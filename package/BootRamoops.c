// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel's log in the stock ramoops console zone (0xB0000000, 2 MiB), for
// CONFIG_XNU_BOOT_PROGRESS with a board whose bring-up kernel links no device
// drivers: the zone writer, the stage lines and the message buffer only.
//
// Adapted from UniXNU-pipa's kits/pipa/BootRamoops.c. That file adds pipa's
// status probes (touch, display, GPU, storage, processes) and its restart into
// fastboot through the PMIC and SPMI, neither of which this board has yet; the
// hooks the kernel patches call are the same.
//
// The zone is a persistent_ram_buffer: the signature "DBGC", the write position
// and the used size, then the ring. The firmware's own RamoopsLogLib writes the
// same zone, and a reader (the phone's Android pstore or a firmware dumper) reads
// it back as console-ramoops-0. Built without XNU's headers
// (scripts/build-boot-progress.py), so the few kernel interfaces it calls are
// declared here.

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

#define ZONE_BASE  0xB0000000UL
#define ZONE_SIZE  0x200000UL
#define ZONE_SIG   0x43474244U                 // "DBGC"
#define ZONE_HEAD  12UL
#define ZONE_DATA  (ZONE_SIZE - ZONE_HEAD)
#define EARLY_SIZE 8192U
#define PASS_MS    100U

extern u64 ml_io_map(u64 phys_addr, u64 size);
extern u64 ml_static_ptovirt(u64 paddr);
// The aperture bases: zero until arm_vm_init runs, so the cards must not be drawn before it.
extern u64 gVirtBase;
extern int kernel_thread_start(void (*continuation)(void *, int), void *parameter, void **new_thread);
extern void thread_deallocate(void *thread);
extern void IOSleep(unsigned milliseconds);
extern int snprintf(char *, unsigned long, const char *, ...);
struct msgbuf {int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc;};
extern struct msgbuf *msgbufp;

#define FB_BASE  0x9C000000UL                 // the panel: 1080x2400, row 4320 bytes, 32 bpp
#define FB_WIDTH 1080U
#define FB_HEIGHT 2400U
#define FB_ROW   (4320U / 4U)
#define CARD     100U                          // one card per stage, a row of them above the middle
#define CARD_TOP 1900U

static volatile u32 *fb;                       // the frame buffer through the physical aperture
static volatile unsigned cards;                // stages drawn so far
static volatile u8 *zone;                       // the zone's mapping, once boot_progress_start made it
static char early[EARLY_SIZE];                  // what came before it
static u32 early_len;
static volatile int lock;
static char last_byte = '\n';                   // the zone's last byte written, for line()
// Written by the panic screen (patch-xnu-entry-probe.py resolves the symbol), 256 bytes, the first panic's message.
char boot_progress_panic_text[256];

// The lock masks interrupts while it is held, so the holder is never preempted.
static u64 take(void) {
  u64 daif;
  __asm__ volatile("mrs %0, daif\n\tmsr daifset, #3" : "=r"(daif) :: "memory");
  while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {}
  return daif;
}
static void give(u64 daif) {
  __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
  __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

// Bytes into the ring, the oldest going once it is full.
static void zone_put(const char *text, unsigned n) {
  volatile u32 *head = (volatile u32 *)zone;
  u32 start = head[1], size = head[2];
  if (head[0] != ZONE_SIG || start >= ZONE_DATA || size > ZONE_DATA) {head[0] = ZONE_SIG; start = 0; size = 0;}
  for (unsigned i = 0; i < n; ++i) {
    zone[ZONE_HEAD + start] = (u8)text[i];
    start = start + 1 == ZONE_DATA ? 0 : start + 1;
  }
  size = size + n > ZONE_DATA ? (u32)ZONE_DATA : size + n;
  head[1] = start;
  head[2] = size;
  if (n) last_byte = text[n - 1];
  __asm__ volatile("dsb sy" ::: "memory");
}

static void put(const char *text, unsigned n) {
  u64 daif = take();
  if (zone) zone_put(text, n);
  else for (unsigned i = 0; i < n && early_len < EARLY_SIZE; ++i) early[early_len++] = text[i];
  give(daif);
}

// A card per stage: the panel is the only output before the kernel console, and
// the console clears the screen when it is created, so the writer thread redraws
// these. The colour says which stage the kernel reached; a photo reads it.
static void cards_draw(void) {
  if (!fb) {
    if (!gVirtBase) return;                 // arm_vm_init has not set the aperture yet
    fb = (volatile u32 *)ml_static_ptovirt(FB_BASE);
    if (!fb) return;
  }
  unsigned count = cards;
  if (count > 10) count = 10;
  for (unsigned i = 0; i < count; ++i) {
    static const u32 palette[10] = {0xFFFFFFFFu, 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u,
                                    0xFFFF00FFu, 0xFF00FFFFu, 0xFFFF8000u, 0xFF8000FFu, 0xFF808080u};
    u32 colour = palette[i];
    for (unsigned y = 0; y < CARD; ++y)
      for (unsigned x = 0; x < CARD; ++x)
        fb[(u64)(CARD_TOP + y) * FB_ROW + 20 + i * (CARD + 5) + x] = colour;
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

// A line of its own: after message buffer text that has not ended its line.
static void line(const char *prefix, const char *text) {
  char buffer[768];
  int n = snprintf(buffer, sizeof buffer, "%s[xnu] %s%s\n", last_byte == '\n' ? "" : "\n", prefix, text ? text : "");
  if (n > 0) put(buffer, (unsigned)n < sizeof buffer ? (unsigned)n : (unsigned)sizeof buffer - 1);
}

// The message buffer's text written since the previous pass (msg_bufx is its next write position); the first pass takes it all.
static void messages(void) {
  static int last = -1;
  struct msgbuf *m = msgbufp;
  if (!m || !m->msg_bufc || m->msg_size <= 0) return;
  const int size = m->msg_size, x = m->msg_bufx;
  if (x < 0 || x >= size) return;
  if (last < 0) last = (x + 1) % size;
  const int n = (x - last + size) % size;
  if (last + n <= size) put(m->msg_bufc + last, (unsigned)n);
  else {
    put(m->msg_bufc + last, (unsigned)(size - last));
    put(m->msg_bufc, (unsigned)(n - (size - last)));
  }
  last = x;
}

// The panic message, once the panic screen has copied it.
static void panic_text(int settled) {
  static int done;
  static unsigned seen;
  volatile const char *source = boot_progress_panic_text;
  if (done || !source[0]) return;
  char text[sizeof boot_progress_panic_text];
  unsigned n = 0;
  for (; n < sizeof text - 1 && source[n]; ++n) text[n] = source[n];
  text[n] = 0;
  if (!settled && n != seen) {seen = n; return;}
  done = 1;
  line("panic: ", text);
}

static void writer(void *argument, int result) {
  (void)argument; (void)result;
  for (;;) {
    messages();
    panic_text(0);
    cards_draw();
    IOSleep(PASS_MS);
  }
}

void boot_progress_init(void) {line("", "boot_progress_init (stages kept until the ramoops zone is mapped)");}

void boot_progress_start(void) {
  volatile u8 *mapped = (volatile u8 *)ml_io_map(ZONE_BASE, ZONE_SIZE);
  if (!mapped) return;
  u64 daif = take();
  zone = mapped;
  static const char banner[] = "\n[xnu] ---- kernel: ramoops console zone mapped; the stages before it follow ----\n";
  zone_put(banner, sizeof banner - 1);
  zone_put(early, early_len);
  give(daif);
  cards = 1;
  cards_draw();
  void *thread = 0;
  if (kernel_thread_start(writer, 0, &thread) == 0) thread_deallocate(thread);
  else line("", "message buffer thread NOT started");
}

int boot_progress_active(void) {return 0;}
void boot_progress_bind(unsigned long base, unsigned width, unsigned height, unsigned rowbytes, unsigned depth) {
  (void)base; (void)width; (void)height; (void)rowbytes; (void)depth;
}
void boot_progress_putc(char c)
{
	/* kprintf reaches the video console slot now: keep the characters visible. */
	extern void kebab_early_putc(char c);
	kebab_early_putc(c);
}
void boot_progress_stage(const char *text) {line("stage: ", text); if (cards < 10) ++cards; cards_draw();}
void boot_progress_irq(u64 pc, u64 cpsr, u64 sp) {(void)pc; (void)cpsr; (void)sp;}
void boot_progress_poll(void) {}
void boot_progress_user_breakpoint(void *state, unsigned esr) {(void)state; (void)esr;}
void boot_progress_user_abort(void *state, unsigned esr, u64 fault_addr) {(void)state; (void)esr; (void)fault_addr;}
void boot_progress_critical_exit(int pid, char *name, unsigned long code) {(void)pid; (void)name; (void)code;}
void boot_progress_process_stage(int pid, const char *name, const char *stage) {
  char text[160];
  snprintf(text, sizeof text, "%s (%d): %s", name ? name : "?", pid, stage ? stage : "?");
  line("process: ", text);
}
void boot_progress_exit(int pid, const char *name, int status, unsigned reason_namespace, u64 reason_code) {
  char text[160];
  snprintf(text, sizeof text, "%s (%d) exited %d, reason %u/%llu", name ? name : "?", pid, status, reason_namespace,
           (unsigned long long)reason_code);
  line("process: ", text);
}
void boot_progress_panic_hook(void) {panic_text(1);}
int boot_progress_prepare_scanout(void) {return 0;}
void boot_progress_cancel_scanout(void) {}
int boot_progress_pulse_anchor(unsigned long base, u64 bytes, unsigned width, unsigned height, unsigned rowbytes) {
  (void)base; (void)bytes; (void)width; (void)height; (void)rowbytes;
  return 0;
}
