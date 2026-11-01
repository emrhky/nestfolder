/*
 * nestfolder kurulum uygulamasi (VPK) - 1.0.0
 *
 * Dil: Vita sistem dili Turkce ise Turkce, degilse Ingilizce (lang.h).
 * Onay/geri tuslari sistemin "Enter" ayarina gore (X ya da O).
 * Her menu secimi once ayrintili bir onay ekrani gosterir.
 *
 * Menu:
 *   Kur / guncelle            eklentiyi ur0:tai/'ye kopyalar, config.txt'ye
 *                             satiri ekler, eski klasorlere titleId verir
 *   Eski klasorlere titleId ver
 *   Kaldir                    ic klasorleri ana menuye cikarir, config
 *                             satirini ve eklenti dosyasini siler
 *   Ilk yedege don            ilk acilista alinan app.db ve config.txt'yi
 *                             geri yukler
 *   Cikis
 *
 * Ilk acilista app.db ve config.txt bir kez ux0:data/nestfolder/first_backup/
 * icine yedeklenir (uygulama silinse de kalir). Eski surumun ilk_yedek/ ve
 * kurulum/ klasorleri yeni adlarina tasinir. Islemler once calisma
 * kopyasinda hazirlanir; geri sayimdan sonra (geri tusu ile iptal edilebilir)
 * yazilir ve Vita hemen yeniden baslatilir.
 */
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/shellutil.h>
#include <psp2/sqlite.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "db.h"
#include "font.h"
#include "lang.h"
#include "vfs_sceio.h"
#include "vsqlite.h"

#define APP_VERSION    "1.0.3"
#define PLUGIN_VERSION "1.0.3"

int g_tr;   /* lang.h */

#define LIVE_DB     "ur0:shell/db/app.db"
#define CFG_UR0     "ur0:tai/config.txt"
#define CFG_UX0     "ux0:tai/config.txt"
#define PLUGIN_SRC  "app0:payload/nestfolder.suprx"
#define PLUGIN_DST  "ur0:tai/nestfolder.suprx"
#define DATA_DIR    "ux0:data/nestfolder"
#define FIRST_DIR   DATA_DIR "/first_backup"
#define FIRST_DB    FIRST_DIR "/app.db"
#define FIRST_UR0   FIRST_DIR "/config_ur0.txt"
#define FIRST_UX0   FIRST_DIR "/config_ux0.txt"
#define FIRST_DONE  FIRST_DIR "/done.txt"
#define WORK_DIR    DATA_DIR "/installer"
#define OLD_FIRST_DIR  DATA_DIR "/ilk_yedek"   /* kurulum 1.0/1.1 */
#define OLD_FIRST_DONE "tamam.txt"
#define OLD_WORK_DIR   DATA_DIR "/kurulum"
#define ORIG_DB     WORK_DIR "/orig.db"
#define WORK_DB     WORK_DIR "/work.db"

#define COUNTDOWN_SEC 5

/* ===================================================================
 *  Ekran: 960x544, 12x24 hucre -> 80 sutun, 22 satir
 * =================================================================== */
#define SCR_W 960
#define SCR_H 544
#define COLS  (SCR_W / FONT_W)
#define ROWS  (SCR_H / FONT_H)

#define C_BG     0xFF2A1E18u   /* ABGR */
#define C_BAR    0xFF6B4A2Cu
#define C_TEXT   0xFFE6E6E6u
#define C_DIM    0xFF9A9A9Au
#define C_OK     0xFF7CD67Cu
#define C_WARN   0xFF4FC8F2u
#define C_ERR    0xFF6A6AF2u
#define C_SEL_BG 0xFFC08A4Au
#define C_WHITE  0xFFFFFFFFu

static uint32_t *fb;

static void fb_init(void)
{
	SceUID blk = sceKernelAllocMemBlock("fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
	                                    0x200000, NULL);
	sceKernelGetMemBlockBase(blk, (void **)&fb);
	SceDisplayFrameBuf f;
	memset(&f, 0, sizeof(f));
	f.size = sizeof(f);
	f.base = fb;
	f.pitch = SCR_W;
	f.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	f.width = SCR_W;
	f.height = SCR_H;
	sceDisplaySetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
	for (int j = y; j < y + h && j < SCR_H; j++)
		for (int i = x; i < x + w && i < SCR_W; i++)
			fb[j * SCR_W + i] = c;
}

static uint32_t blend(uint32_t bg, uint32_t fg, unsigned a)
{
	uint32_t r = 0xFF000000u;
	for (int s = 0; s < 24; s += 8) {
		unsigned b = (bg >> s) & 0xFF, f = (fg >> s) & 0xFF;
		r |= ((b * (255 - a) + f * a) / 255) << s;
	}
	return r;
}

static int glyph_index(unsigned cp)
{
	for (int i = 0; i < FONT_COUNT; i++) if (font_cp[i] == cp) return i;
	return -1;
}

static void draw_glyph(int col, int row, unsigned cp, uint32_t fg, uint32_t bg)
{
	if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
	int gi = glyph_index(cp);
	if (gi < 0) gi = glyph_index('?');
	int x0 = col * FONT_W, y0 = row * FONT_H;
	const unsigned char *g = font_px[gi];
	for (int y = 0; y < FONT_H; y++)
		for (int x = 0; x < FONT_W; x++)
			fb[(y0 + y) * SCR_W + x0 + x] = g[y * FONT_W + x] ? blend(bg, fg, g[y * FONT_W + x]) : bg;
}

/* UTF-8 metni yazar; yazilan hucre sayisini dondurur */
static int draw_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
	const unsigned char *p = (const unsigned char *)s;
	int c = col;
	while (*p && c < COLS) {
		unsigned cp = *p++;
		if (cp >= 0xC0) {
			int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
			cp &= 0x3F >> extra;
			while (extra-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
		} else if (cp >= 0x80) cp = '?';
		draw_glyph(c++, row, cp, fg, bg);
	}
	return c - col;
}

static void clear_row(int row, uint32_t bg) { fill_rect(0, row * FONT_H, SCR_W, FONT_H, bg); }

static void draw_line(int row, const char *s, uint32_t fg, uint32_t bg)
{
	clear_row(row, bg);
	draw_text(1, row, s, fg, bg);
}

static void draw_title(const char *right)
{
	char buf[96];
	clear_row(0, C_BAR);
	snprintf(buf, sizeof(buf), TR("nestfolder installer %s · plugin %s", "nestfolder kurulum %s · eklenti %s"),
	         APP_VERSION, PLUGIN_VERSION);
	draw_text(1, 0, buf, C_WHITE, C_BAR);
	if (right) {
		int n = 0;
		for (const unsigned char *p = (const unsigned char *)right; *p; p++) if ((*p & 0xC0) != 0x80) n++;
		draw_text(COLS - 1 - n, 0, right, C_WHITE, C_BAR);
	}
}

static void draw_help(const char *s)
{
	clear_row(ROWS - 1, C_BAR);
	draw_text(1, ROWS - 1, s, C_WHITE, C_BAR);
}

/* ===================================================================
 *  Islem ekrani: kayan log (satir 2 .. ROWS-2)
 * =================================================================== */
#define LOG_TOP 2
#define LOG_ROWS (ROWS - 2 - LOG_TOP)   /* ROWS-2: geri sayim satiri */
static char log_txt[LOG_ROWS][COLS * 4 + 4];
static uint32_t log_col[LOG_ROWS];
static int log_n;

static void log_redraw(void)
{
	for (int i = 0; i < LOG_ROWS; i++) {
		clear_row(LOG_TOP + i, C_BG);
		if (i < log_n) draw_text(1, LOG_TOP + i, log_txt[i], log_col[i], C_BG);
	}
}

static void log_c(uint32_t col, const char *fmt, ...)
{
	char buf[sizeof(log_txt[0])];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	/* Ekrana sigmayan satiri kelime sinirindan bol. "• " ya da bosluklarla
	 * baslayan satirin devami ayni hizada girintilenir. */
	int indent = 0;
	if (!strncmp(buf, "• ", 4)) indent = 2;
	else while (buf[indent] == ' ' && indent < 8) indent++;
	int first = 1;
	char *s = buf;
	do {
		const int pad = first ? 0 : indent;
		const int maxw = COLS - 2 - pad;
		int chars = 0, cut = 0, last_space = -1, i = 0;
		for (; s[i]; i++) {
			if (((unsigned char)s[i] & 0xC0) == 0x80) continue;
			if (chars == maxw) break;
			if (s[i] == ' ') last_space = i;
			chars++;
		}
		cut = s[i] ? (last_space > 0 ? last_space : i) : i;
		if (log_n == LOG_ROWS) {
			memmove(log_txt[0], log_txt[1], sizeof(log_txt[0]) * (LOG_ROWS - 1));
			memmove(&log_col[0], &log_col[1], sizeof(log_col[0]) * (LOG_ROWS - 1));
			log_n--;
		}
		memset(log_txt[log_n], ' ', pad);
		memcpy(log_txt[log_n] + pad, s, cut);
		log_txt[log_n][pad + cut] = 0;
		first = 0;
		log_col[log_n] = col;
		log_n++;
		s += cut;
		while (*s == ' ') s++;
	} while (*s);
	log_redraw();
}
#define LOG(...)  log_c(C_TEXT, __VA_ARGS__)
#define LOGOK(...)  log_c(C_OK, __VA_ARGS__)
#define LOGW(...) log_c(C_WARN, __VA_ARGS__)
#define LOGE(...) log_c(C_ERR, __VA_ARGS__)

static void db_log(const char *m) { log_c(C_DIM, "%s", m); }

static void op_screen(const char *title)
{
	fill_rect(0, 0, SCR_W, SCR_H, C_BG);
	draw_title(NULL);
	draw_line(1, title, C_WARN, C_BG);
	log_n = 0;
	log_redraw();
	draw_help(TR("Please wait...", "Lütfen bekle..."));
}

/* ===================================================================
 *  Tuslar
 * =================================================================== */
static uint32_t btn_prev;
/* Onay ve geri tuslari: sistemin Enter ayari (Japon modeller O ile onaylar) */
static uint32_t BTN_OK = SCE_CTRL_CROSS, BTN_BACK = SCE_CTRL_CIRCLE;
static const char *K_OK = "X", *K_BACK = "O";

static uint32_t buttons_now(void)
{
	SceCtrlData c;
	memset(&c, 0, sizeof(c));
	sceCtrlPeekBufferPositive(0, &c, 1);
	return c.buttons;
}

/* Yeni basilan tuslar */
static uint32_t buttons_pressed(void)
{
	uint32_t b = buttons_now();
	uint32_t p = b & ~btn_prev;
	btn_prev = b;
	return p;
}

static uint32_t wait_buttons(uint32_t mask)
{
	for (;;) {
		uint32_t p = buttons_pressed() & mask;
		if (p) return p;
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
		sceDisplayWaitVblankStart();
	}
}

/* ===================================================================
 *  Dosya yardimcilari
 * =================================================================== */
static uint8_t io_buf[64 * 1024];

static int exists(const char *p)
{
	SceIoStat st;
	return sceIoGetstat(p, &st) >= 0;
}

static void mkdirs(void)
{
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir(DATA_DIR, 0777);
	sceIoMkdir(FIRST_DIR, 0777);
	sceIoMkdir(WORK_DIR, 0777);
}

/* FNV-1a 64 + boyut */
typedef struct { uint64_t h; int64_t size; } fsum_t;

static int file_sum(const char *path, fsum_t *s)
{
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) return -1;
	uint64_t h = 1469598103934665603ull;
	int64_t total = 0;
	int n;
	while ((n = sceIoRead(fd, io_buf, sizeof(io_buf))) > 0) {
		for (int i = 0; i < n; i++) { h ^= io_buf[i]; h *= 1099511628211ull; }
		total += n;
	}
	sceIoClose(fd);
	if (n < 0) return -1;
	s->h = h; s->size = total;
	return 0;
}

static int sum_eq(const fsum_t *a, const fsum_t *b) { return a->h == b->h && a->size == b->size; }

static int copy_file(const char *from, const char *to)
{
	SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
	if (in < 0) return -1;
	SceUID out = sceIoOpen(to, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (out < 0) { sceIoClose(in); return -1; }
	int n, ok = 1;
	while ((n = sceIoRead(in, io_buf, sizeof(io_buf))) > 0)
		if (sceIoWrite(out, io_buf, n) != n) { ok = 0; break; }
	if (n < 0) ok = 0;
	sceIoClose(in);
	if (sceIoClose(out) < 0) ok = 0;
	return ok ? 0 : -1;
}

/* Kopyala ve dogrula. Kaynak kopyalama sirasinda degistiyse tekrar dene. */
static int copy_verified(const char *from, const char *to, fsum_t *out_sum)
{
	for (int t = 0; t < 3; t++) {
		fsum_t a, b;
		if (copy_file(from, to) < 0) continue;
		if (file_sum(to, &a) < 0 || file_sum(from, &b) < 0) continue;
		if (sum_eq(&a, &b)) { if (out_sum) *out_sum = a; return 0; }
		sceKernelDelayThread(300 * 1000);
	}
	return -1;
}

static char *read_text(const char *path)
{
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) return NULL;
	SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	if (size < 0 || size > 1024 * 1024) { sceIoClose(fd); return NULL; }
	char *b = malloc((size_t)size + 1);
	if (!b) { sceIoClose(fd); return NULL; }
	int n = sceIoRead(fd, b, (SceSize)size);
	sceIoClose(fd);
	if (n != (int)size) { free(b); return NULL; }
	b[size] = 0;
	return b;
}

static int write_all(const char *path, const void *data, int len)
{
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0) return -1;
	int w = sceIoWrite(fd, data, len);
	int c = sceIoClose(fd);
	return (w == len && c >= 0) ? 0 : -1;
}

static int same_content(const char *path, const void *data, int len)
{
	char *t = read_text(path);
	if (!t) return 0;
	int ok = (int)strlen(t) == len && memcmp(t, data, len) == 0;
	free(t);
	return ok;
}

/* Metin dosyasini guvenli degistir: tmp'ye yaz, dogrula, eskisini .old
 * yapip tmp'yi yerine koy. Basarisizlikta eskisini geri koyar. */
static int replace_text(const char *path, const char *text)
{
	char tmp[96], old[96];
	int len = (int)strlen(text);
	snprintf(tmp, sizeof(tmp), "%s.nest_tmp", path);
	snprintf(old, sizeof(old), "%s.nest_old", path);
	sceIoRemove(tmp);
	if (write_all(tmp, text, len) < 0 || !same_content(tmp, text, len)) { sceIoRemove(tmp); return -1; }
	int had = exists(path);
	sceIoRemove(old);
	if (had && sceIoRename(path, old) < 0) { sceIoRemove(tmp); return -1; }
	if (sceIoRename(tmp, path) < 0) {
		if (had) sceIoRename(old, path);
		sceIoRemove(tmp);
		return -1;
	}
	sceIoRemove(old);
	return same_content(path, text, len) ? 0 : -1;
}

/* Ikili dosyayi guvenli degistir (eklenti dosyasi) */
static int replace_binary(const char *src, const char *dst)
{
	char tmp[96], old[96];
	fsum_t a, b;
	snprintf(tmp, sizeof(tmp), "%s.nest_tmp", dst);
	snprintf(old, sizeof(old), "%s.nest_old", dst);
	sceIoRemove(tmp);
	if (copy_verified(src, tmp, &a) < 0) { sceIoRemove(tmp); return -1; }
	int had = exists(dst);
	sceIoRemove(old);
	if (had && sceIoRename(dst, old) < 0) { sceIoRemove(tmp); return -1; }
	if (sceIoRename(tmp, dst) < 0) {
		if (had) sceIoRename(old, dst);
		sceIoRemove(tmp);
		return -1;
	}
	sceIoRemove(old);
	return (file_sum(dst, &b) == 0 && sum_eq(&a, &b)) ? 0 : -1;
}

/* ===================================================================
 *  app.db: anlik kopya ve geri yazma
 * =================================================================== */
static fsum_t orig_sum;   /* calisma kopyasi alinirken canli app.db */

/* Shell app.db'ye yazarken (sicak gunluk) bekle. -1: hala yaziyor; -2: WAL */
static int wait_db_idle(void)
{
	static const uint8_t magic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
	if (exists(LIVE_DB "-wal")) return -2;
	for (int t = 0; t < 30; t++) {
		uint8_t hdr[8];
		SceUID fd = sceIoOpen(LIVE_DB "-journal", SCE_O_RDONLY, 0);
		if (fd < 0) return 0;
		int n = sceIoRead(fd, hdr, 8);
		sceIoClose(fd);
		if (n < 8 || memcmp(hdr, magic, 8) != 0) return 0;
		sceKernelDelayThread(100 * 1000);
	}
	return -1;
}

static int snapshot_live(void)
{
	int w = wait_db_idle();
	if (w == -2) { LOGE(TR("app.db-wal found; unexpected state, nothing done.", "app.db-wal bulundu; beklenmeyen durum, işlem yapılmadı.")); return -1; }
	if (w < 0) { LOGE(TR("Shell has not finished writing app.db, try again shortly.", "Shell app.db'ye yazmayı bitirmedi, biraz sonra tekrar dene.")); return -1; }
	sceIoRemove(WORK_DB "-journal");
	if (copy_verified(LIVE_DB, ORIG_DB, &orig_sum) < 0) {
		LOGE(TR("Cannot copy app.db (%s).", "app.db kopyalanamadı (%s)."), LIVE_DB);
		return -1;
	}
	if (copy_file(ORIG_DB, WORK_DB) < 0) {
		LOGE(TR("Cannot create the working copy.", "Çalışma kopyası oluşturulamadı."));
		return -1;
	}
	return 0;
}

static int write_live_db(const char *src, const fsum_t *want)
{
	for (int t = 0; t < 2; t++) {
		fsum_t got;
		if (copy_file(src, LIVE_DB) == 0 && file_sum(LIVE_DB, &got) == 0 && sum_eq(&got, want))
			return 0;
	}
	return -1;
}

/* Canli app.db'yi src ile degistir. Basarisizlikta ORIG_DB geri yazilir. */
static int commit_db(const char *src, int check_unchanged)
{
	fsum_t now, want;
	int w = wait_db_idle();
	if (w < 0) { LOGE(TR("Shell is writing app.db, write postponed. Try again.", "Shell app.db'ye yazıyor, yazma ertelendi. Tekrar dene.")); return -1; }
	if (check_unchanged) {
		if (file_sum(LIVE_DB, &now) < 0) { LOGE(TR("Cannot read app.db.", "app.db okunamadı.")); return -1; }
		if (!sum_eq(&now, &orig_sum)) {
			LOGE(TR("app.db changed in the meantime; nothing written. Run the operation again.",
			        "app.db bu arada değişti; değişiklik yazılmadı. İşlemi tekrar çalıştır."));
			return -1;
		}
	}
	if (file_sum(src, &want) < 0) { LOGE(TR("Cannot read the file to write: %s", "Yazılacak dosya okunamadı: %s"), src); return -1; }
	if (write_live_db(src, &want) == 0) return 0;
	LOGE(TR("Cannot write app.db, restoring the previous one...", "app.db yazılamadı, eski hali geri yükleniyor..."));
	if (write_live_db(ORIG_DB, &orig_sum) == 0) LOGW(TR("Previous app.db restored.", "Eski app.db geri yüklendi."));
	else LOGE(TR("Restore failed too! Copy %s to %s with VitaShell.",
	             "Geri yükleme de başarısız! VitaShell ile %s dosyasını %s olarak kopyala."),
	          FIRST_DB, LIVE_DB);
	return -1;
}

/* ===================================================================
 *  Durum
 * =================================================================== */
typedef struct {
	int plugin;          /* 0 yok, 1 guncel, 2 farkli surum */
	const char *cfg;     /* etkin config yolu */
	int cfg_line;        /* eklenti satiri *main'de */
	int db_ok;
	db_stats_t st;
	int first;           /* ilk yedek var */
	char db_err[128];
} status_t;

static status_t S;

static const char *active_cfg(void) { return exists(CFG_UX0) ? CFG_UX0 : CFG_UR0; }

static void refresh_status(void)
{
	memset(&S, 0, sizeof(S));
	fsum_t a, b;
	if (exists(PLUGIN_DST))
		S.plugin = (file_sum(PLUGIN_DST, &a) == 0 && file_sum(PLUGIN_SRC, &b) == 0 && sum_eq(&a, &b)) ? 1 : 2;
	S.cfg = active_cfg();
	char *t = read_text(S.cfg);
	if (t) { S.cfg_line = cfg_plugin_in_main(t); free(t); }
	S.first = exists(FIRST_DONE);
	if (wait_db_idle() == 0 && copy_verified(LIVE_DB, WORK_DB, NULL) == 0) {
		S.db_ok = db_stats(WORK_DB, &S.st) == 0;
		if (!S.db_ok) snprintf(S.db_err, sizeof(S.db_err), "%s", db_error());
	} else {
		snprintf(S.db_err, sizeof(S.db_err), "%s", TR("cannot read app.db", "app.db okunamadı"));
	}
}

/* ===================================================================
 *  Ilk yedek (bir kez)
 * =================================================================== */
/* Kurulum 1.0/1.1 klasorlerini yeni adlara tasi. Dizin yeniden adlandirilamazsa
 * dosyalar kopyalanir; eski klasor yerinde kalir. */
static void migrate_old_dirs(void)
{
	if (exists(OLD_FIRST_DIR) && !exists(FIRST_DIR)) {
		if (sceIoRename(OLD_FIRST_DIR, FIRST_DIR) < 0) {
			static const char *const files[] = {"app.db", "config_ur0.txt", "config_ux0.txt", OLD_FIRST_DONE};
			sceIoMkdir(FIRST_DIR, 0777);
			for (int i = 0; i < 4; i++) {
				char a[128], b[128];
				snprintf(a, sizeof(a), OLD_FIRST_DIR "/%s", files[i]);
				snprintf(b, sizeof(b), FIRST_DIR "/%s", files[i]);
				if (exists(a)) copy_verified(a, b, NULL);
			}
		}
	}
	char old_done[128];
	snprintf(old_done, sizeof(old_done), FIRST_DIR "/%s", OLD_FIRST_DONE);
	if (exists(old_done) && !exists(FIRST_DONE)) sceIoRename(old_done, FIRST_DONE);
	if (exists(OLD_WORK_DIR) && !exists(WORK_DIR)) sceIoRename(OLD_WORK_DIR, WORK_DIR);
}

static int first_backup(void)
{
	if (exists(FIRST_DONE)) return 0;
	mkdirs();
	if (wait_db_idle() != 0) return -2;
	if (copy_verified(LIVE_DB, FIRST_DB, NULL) < 0) return -1;
	int ur0 = exists(CFG_UR0), ux0 = exists(CFG_UX0);
	sceIoRemove(FIRST_UR0);
	sceIoRemove(FIRST_UX0);
	if (ur0 && copy_verified(CFG_UR0, FIRST_UR0, NULL) < 0) return -1;
	if (ux0 && copy_verified(CFG_UX0, FIRST_UX0, NULL) < 0) return -1;
	char info[128];
	snprintf(info, sizeof(info), "nestfolder installer %s first backup\nconfig_ur0=%d\nconfig_ux0=%d\n",
	         APP_VERSION, ur0, ux0);
	return write_all(FIRST_DONE, info, (int)strlen(info));
}

/* ===================================================================
 *  Plan: hazirlanan degisiklikler, geri sayimdan sonra sirayla uygulanir
 * =================================================================== */
typedef struct {
	const char *db_src;      /* canli app.db'ye yazilacak dosya (NULL: yok) */
	int db_check;            /* yazmadan once canli dosya degismemis olmali */
	int copy_plugin;
	char *cfg_new[2];        /* [0] ur0, [1] ux0 icin yeni icerik */
	int delete_plugin;
	int nchanges;
} plan_t;

static const char *const cfg_paths[2] = {CFG_UR0, CFG_UX0};

static void plan_free(plan_t *p)
{
	free(p->cfg_new[0]);
	free(p->cfg_new[1]);
	memset(p, 0, sizeof(*p));
}

/* Yeni config icerigini plana koy (degisiklik yoksa koymaz) */
static int plan_cfg(plan_t *p, int which, char *newtext, const char *oldtext)
{
	if (!newtext) { LOGE(TR("Out of memory.", "Bellek yetersiz.")); return -1; }
	if (oldtext && strcmp(newtext, oldtext) == 0) { free(newtext); return 0; }
	p->cfg_new[which] = newtext;
	p->nchanges++;
	return 0;
}

static int execute_plan(plan_t *p)
{
	sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN | SCE_SHELL_UTIL_LOCK_TYPE_POWEROFF_MENU);
	int r = -1;
	if (p->db_src) {
		LOG(TR("Writing app.db...", "app.db yazılıyor..."));
		if (commit_db(p->db_src, p->db_check) < 0) goto out;
		LOGOK(TR("app.db written and verified.", "app.db yazıldı ve doğrulandı."));
	}
	if (p->copy_plugin) {
		sceIoMkdir("ur0:tai", 0777);
		if (replace_binary(PLUGIN_SRC, PLUGIN_DST) < 0) { LOGE(TR("Cannot copy plugin: %s", "Eklenti kopyalanamadı: %s"), PLUGIN_DST); goto out; }
		LOGOK(TR("Plugin copied: %s", "Eklenti kopyalandı: %s"), PLUGIN_DST);
	}
	for (int i = 0; i < 2; i++) {
		if (!p->cfg_new[i]) continue;
		if (replace_text(cfg_paths[i], p->cfg_new[i]) < 0) { LOGE(TR("Cannot write %s.", "%s yazılamadı."), cfg_paths[i]); goto out; }
		LOGOK(TR("%s updated.", "%s güncellendi."), cfg_paths[i]);
	}
	if (p->delete_plugin && exists(PLUGIN_DST)) {
		if (sceIoRemove(PLUGIN_DST) < 0) LOGW(TR("Cannot delete %s (harmless: it is no longer loaded).",
		                                         "%s silinemedi (zararsız: artık yüklenmiyor)."), PLUGIN_DST);
		else LOGOK(TR("%s deleted.", "%s silindi."), PLUGIN_DST);
	}
	r = 0;
out:
	sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN | SCE_SHELL_UTIL_LOCK_TYPE_POWEROFF_MENU);
	return r;
}

/* Geri sayim; geri tusu ile iptal. Iptal edilirse 0, devam edilirse 1. */
static int countdown(void)
{
	btn_prev = buttons_now();
	for (int s = COUNTDOWN_SEC; s > 0; s--) {
		char buf[96];
		char help[48];
		snprintf(buf, sizeof(buf), TR("Changes will be written and the Vita restarted in %d s.",
		                              "%d sn sonra değişiklikler yazılıp Vita yeniden başlatılacak."), s);
		draw_line(ROWS - 2, buf, C_WARN, C_BG);
		snprintf(help, sizeof(help), TR("%s: cancel", "%s: iptal"), K_BACK);
		draw_help(help);
		for (int f = 0; f < 60; f++) {
			if (buttons_pressed() & BTN_BACK) {
				draw_line(ROWS - 2, TR("Cancelled, nothing was written.", "İptal edildi, hiçbir şey yazılmadı."), C_WARN, C_BG);
				return 0;
			}
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
			sceDisplayWaitVblankStart();
		}
	}
	clear_row(ROWS - 2, C_BG);
	return 1;
}

static void reboot_now(void)
{
	LOGOK(TR("Restarting...", "Yeniden başlatılıyor..."));
	draw_help("");
	sceKernelDelayThread(800 * 1000);
	scePowerRequestColdReset();
	for (;;) sceKernelDelayThread(1000 * 1000);
}

/* Plan bos degilse geri say, uygula, yeniden baslat. Donerse menuye donulur.
 * failed: hazirlik basarisiz oldu, hicbir sey uygulanmaz. */
static void finish(plan_t *p, int failed)
{
	if (failed) {
		LOGE(TR("Nothing done; no file was changed.", "İşlem yapılmadı; hiçbir dosya değiştirilmedi."));
	} else if (p->nchanges == 0) {
		LOGOK(TR("No changes needed; no restart.", "Değişiklik gerekmiyor; yeniden başlatma yok."));
	} else if (countdown()) {
		if (execute_plan(p) == 0) { plan_free(p); reboot_now(); }
		LOGE(TR("Operation stopped halfway; the Vita was not restarted.", "İşlem yarıda kaldı; Vita yeniden başlatılmadı."));
	}
	plan_free(p);
	char help[64];
	snprintf(help, sizeof(help), TR("%s / %s: back to menu", "%s / %s: menüye dön"), K_OK, K_BACK);
	draw_help(help);
	wait_buttons(BTN_OK | BTN_BACK);
}

/* ===================================================================
 *  Islemler
 * =================================================================== */

/* titleId'leri calisma kopyasinda ver; plana ekle. Donus <0: hata */
static int prepare_titleids(plan_t *p)
{
	int changed = 0;
	if (db_fix_titleids(WORK_DB, &changed, db_log) < 0) {
		LOGE(TR("Cannot assign titleIds: %s", "titleId verilemedi: %s"), db_error());
		return -1;
	}
	if (changed) {
		LOGOK(TR("titleId assigned to %d folder(s).", "%d klasöre titleId verildi."), changed);
		p->db_src = WORK_DB;
		p->db_check = 1;
		p->nchanges++;
	} else {
		LOG(TR("No folder without a titleId.", "titleId'si boş klasör yok."));
	}
	return 0;
}

static void op_install(void)
{
	plan_t p;
	memset(&p, 0, sizeof(p));
	op_screen(TR("Install / update", "Kur / güncelle"));

	if (!exists(PLUGIN_SRC)) { LOGE(TR("Plugin missing from the package: %s", "Paket içinde eklenti yok: %s"), PLUGIN_SRC); finish(&p, 1); return; }
	fsum_t a, b;
	if (exists(PLUGIN_DST) && file_sum(PLUGIN_DST, &a) == 0 && file_sum(PLUGIN_SRC, &b) == 0 && sum_eq(&a, &b)) {
		LOG(TR("Plugin file is already up to date.", "Eklenti dosyası zaten güncel."));
	} else {
		LOG(exists(PLUGIN_DST) ? TR("Plugin will be updated to %s.", "Eklenti %s sürümüne güncellenecek.")
		                       : TR("Plugin %s will be installed.", "Eklenti %s kurulacak."), PLUGIN_VERSION);
		p.copy_plugin = 1;
		p.nchanges++;
	}

	/* Etkin config'e satiri ekle; diger config'te satir varsa temizle */
	int ai = strcmp(active_cfg(), CFG_UX0) == 0 ? 1 : 0;
	if (ai == 1) LOGW(TR("ux0:tai/config.txt exists: taiHEN uses it, the line goes there.",
	                     "ux0:tai/config.txt var: taiHEN onu kullanıyor, satır oraya eklenecek."));
	char *t = read_text(cfg_paths[ai]);
	if (!t && exists(cfg_paths[ai])) { LOGE(TR("Cannot read %s.", "%s okunamadı."), cfg_paths[ai]); finish(&p, 1); return; }
	if (!t) LOGW(TR("%s does not exist, it will be created.", "%s yok, yeni oluşturulacak."), cfg_paths[ai]);
	if (plan_cfg(&p, ai, cfg_add_plugin(t ? t : ""), t) < 0) { free(t); finish(&p, 1); return; }
	LOG(p.cfg_new[ai] ? TR("%s: line will be added under *main.", "%s: *main altına satır eklenecek.")
	                  : TR("%s: line already present.", "%s: satır zaten var."), cfg_paths[ai]);
	free(t);
	char *o = read_text(cfg_paths[1 - ai]);
	if (o && cfg_has_plugin(o)) {
		if (plan_cfg(&p, 1 - ai, cfg_remove_plugin(o), o) < 0) { free(o); finish(&p, 1); return; }
		LOG(TR("%s: unused plugin line will be removed.", "%s: kullanılmayan eklenti satırı temizlenecek."), cfg_paths[1 - ai]);
	}
	free(o);

	LOG(TR("Checking old folders...", "Eski klasörler kontrol ediliyor..."));
	if (snapshot_live() < 0 || prepare_titleids(&p) < 0) { finish(&p, 1); return; }
	finish(&p, 0);
}

static void op_titleids(void)
{
	plan_t p;
	memset(&p, 0, sizeof(p));
	op_screen(TR("Give titleIds to old folders", "Eski klasörlere titleId ver"));
	finish(&p, snapshot_live() < 0 || prepare_titleids(&p) < 0);
}

static void op_uninstall(void)
{
	plan_t p;
	memset(&p, 0, sizeof(p));
	op_screen(TR("Uninstall", "Kaldır"));

	if (snapshot_live() < 0) { finish(&p, 1); return; }
	int moved = 0, r = db_unnest_all(WORK_DB, &moved, db_log);
	if (r == -2) { LOGE("%s", db_error()); LOGE(TR("Uninstall not done.", "Kaldırma yapılmadı.")); finish(&p, 1); return; }
	if (r < 0) { LOGE(TR("Cannot move nested folders out: %s", "İç klasörler çıkarılamadı: %s"), db_error()); finish(&p, 1); return; }
	if (moved) {
		LOGOK(TR("%d nested folder(s) will be moved to the home screen.", "%d iç klasör ana menüye çıkarılacak."), moved);
	} else {
		LOG(TR("No nested folders.", "İç klasör yok."));
	}
	/* Ic klasorler cikarildiktan sonra eklentinin titleId'lerini temizle */
	int cleared = 0;
	if (db_clear_titleids(WORK_DB, &cleared, db_log) < 0) {
		LOGE(TR("Cannot clear folder titleIds: %s", "Klasör titleId'leri temizlenemedi: %s"), db_error());
		finish(&p, 1);
		return;
	}
	if (cleared)
		LOGOK(TR("titleIds given by the plugin will be cleared (%d folder(s)).",
		         "Eklentinin verdiği titleId'ler temizlenecek (%d klasör)."), cleared);
	if (moved || cleared) {
		p.db_src = WORK_DB;
		p.db_check = 1;
		p.nchanges++;
	}

	for (int i = 0; i < 2; i++) {
		char *t = read_text(cfg_paths[i]);
		if (t && cfg_has_plugin(t)) {
			if (plan_cfg(&p, i, cfg_remove_plugin(t), t) < 0) { free(t); finish(&p, 1); return; }
			LOG(TR("%s: plugin line will be removed.", "%s: eklenti satırı silinecek."), cfg_paths[i]);
		}
		free(t);
	}
	if (exists(PLUGIN_DST)) {
		p.delete_plugin = 1;
		p.nchanges++;
		LOG(TR("%s will be deleted.", "%s silinecek."), PLUGIN_DST);
	}
	LOG(TR("Backups and logs (%s) are kept.", "Yedekler ve loglar (%s) silinmez."), DATA_DIR);
	finish(&p, 0);
}

static void op_restore(void)
{
	plan_t p;
	memset(&p, 0, sizeof(p));
	op_screen(TR("Restore first backup", "İlk yedeğe dön"));

	if (!exists(FIRST_DONE) || !exists(FIRST_DB)) { LOGE(TR("First backup not found (%s).", "İlk yedek bulunamadı (%s)."), FIRST_DIR); finish(&p, 1); return; }
	char *info = read_text(FIRST_DONE);
	int had_ur0 = info && strstr(info, "config_ur0=1") != NULL;
	int had_ux0 = info && strstr(info, "config_ux0=1") != NULL;
	free(info);

	/* Geri yukleme basarisiz olursa donulecek kopya */
	if (snapshot_live() < 0) { finish(&p, 1); return; }
	fsum_t a;
	if (file_sum(FIRST_DB, &a) == 0 && !sum_eq(&a, &orig_sum)) {
		p.db_src = FIRST_DB;
		p.db_check = 0;
		p.nchanges++;
		LOG(TR("app.db will be restored from the first backup.", "app.db ilk yedekten geri yüklenecek."));
		LOGW(TR("Note: bubbles of apps installed after the first backup may disappear.",
		        "Not: ilk yedekten sonra kurulan uygulamaların balonları kaybolabilir."));
	} else {
		LOG(TR("app.db is the same as the first backup.", "app.db ilk yedekle aynı."));
	}

	const char *saved[2] = {FIRST_UR0, FIRST_UX0};
	int had[2] = {had_ur0, had_ux0};
	for (int i = 0; i < 2; i++) {
		char *cur = read_text(cfg_paths[i]);
		char *nw = NULL;
		if (had[i]) nw = read_text(saved[i]);
		else if (cur && cfg_has_plugin(cur)) nw = cfg_remove_plugin(cur);  /* yedekte yoktu */
		if (nw) {
			if (plan_cfg(&p, i, nw, cur) < 0) { free(cur); finish(&p, 1); return; }
			if (p.cfg_new[i]) LOG(TR("%s will be restored.", "%s geri yüklenecek."), cfg_paths[i]);
		} else if (had[i]) {
			LOGE(TR("Cannot read %s.", "%s okunamadı."), saved[i]);
			free(cur);
			finish(&p, 1);
			return;
		}
		free(cur);
	}
	finish(&p, 0);
}

/* ===================================================================
 *  Onay ekranlari: islem secildikten sonra ne yapilacagini ayrintili
 *  gosterir. Onay tusu: devam (1). Geri tusu: hicbir sey yapilmaz (0).
 * =================================================================== */
static void confirm_begin(const char *title)
{
	char buf[160];
	fill_rect(0, 0, SCR_W, SCR_H, C_BG);
	draw_title(NULL);
	snprintf(buf, sizeof(buf), TR("Confirm: %s", "Onay: %s"), title);
	draw_line(1, buf, C_WARN, C_BG);
	log_n = 0;
	log_redraw();
}

static int confirm_end(void)
{
	char help[96];
	snprintf(help, sizeof(help), TR("%s: confirm   %s: back (nothing is done)",
	                                "%s: onayla   %s: geri (hiçbir şey yapılmaz)"), K_OK, K_BACK);
	draw_help(help);
	btn_prev = buttons_now();
	return (wait_buttons(BTN_OK | BTN_BACK) & BTN_OK) ? 1 : 0;
}

/* Ortak son satirlar: geri sayim ve yeniden baslatma */
static void confirm_restart_note(void)
{
	LOG(TR("• Changes are prepared on a working copy and listed on screen. After a "
	       "%d s countdown they are written and the Vita restarts. During the "
	       "countdown %s still cancels.",
	       "• Değişiklikler önce bir çalışma kopyasında hazırlanıp ekranda listelenir. "
	       "%d sn geri sayımdan sonra yazılır ve Vita yeniden başlatılır. Geri "
	       "sayımda %s ile hâlâ iptal edebilirsin."), COUNTDOWN_SEC, K_BACK);
}

static int confirm_install(void)
{
	confirm_begin(TR("Install / update", "Kur / güncelle"));
	LOGW(TR("If you confirm, the following will be done:", "Onaylarsan şunlar yapılacak:"));
	if (S.plugin == 1)
		LOG(TR("• Plugin file is already version %s; it will not be copied.",
		       "• Eklenti dosyası zaten %s sürümünde; kopyalanmayacak."), PLUGIN_VERSION);
	else if (S.plugin == 2)
		LOG(TR("• %s will be replaced with version %s.",
		       "• %s, %s sürümüyle değiştirilecek."), PLUGIN_DST, PLUGIN_VERSION);
	else
		LOG(TR("• nestfolder.suprx (%s) will be copied to ur0:tai/.",
		       "• nestfolder.suprx (%s) ur0:tai/ klasörüne kopyalanacak."), PLUGIN_VERSION);
	if (S.cfg_line)
		LOG(TR("• %s already loads the plugin; it is left as is.",
		       "• %s eklentiyi zaten yüklüyor; olduğu gibi kalır."), S.cfg);
	else
		LOG(TR("• The line ur0:tai/nestfolder.suprx will be added under *main in %s. "
		       "Other lines are not touched.",
		       "• %s içinde *main altına ur0:tai/nestfolder.suprx satırı eklenecek. "
		       "Diğer satırlara dokunulmaz."), S.cfg);
	if (S.db_ok && S.st.no_tid)
		LOG(TR("• %d folder(s) without a titleId get one (FLDRnnnnn); %d of them are "
		       "hidden nested folders, which will appear again.",
		       "• titleId'si boş %d klasöre FLDRnnnnn biçiminde titleId verilecek; bunların "
		       "%d tanesi şu an görünmeyen iç klasör, yeniden görünecek."),
		    S.st.no_tid, S.st.nested_no_tid);
	else
		LOG(TR("• No folder without a titleId; app.db is not changed.",
		       "• titleId'si boş klasör yok; app.db değişmez."));
	confirm_restart_note();
	LOG(TR("• The first backup (%s) is not changed.", "• İlk yedek (%s) değiştirilmez."), FIRST_DIR);
	return confirm_end();
}

static int confirm_titleids(void)
{
	confirm_begin(TR("Give titleIds to old folders", "Eski klasörlere titleId ver"));
	LOGW(TR("If you confirm, the following will be done:", "Onaylarsan şunlar yapılacak:"));
	if (S.db_ok && S.st.no_tid) {
		LOG(TR("• %d folder(s) without a titleId get a unique titleId (FLDRnnnnn).",
		       "• titleId'si boş %d klasöre benzersiz bir titleId (FLDRnnnnn) verilecek."), S.st.no_tid);
		if (S.st.nested_no_tid)
			LOG(TR("• %d nested folder(s) hidden because they had no titleId will appear again.",
			       "• titleId'si olmadığı için görünmeyen %d iç klasör yeniden görünecek."),
			    S.st.nested_no_tid);
		LOG(TR("• Without a titleId a folder cannot be put into another folder; after this it can.",
		       "• titleId'si olmayan klasör başka bir klasöre konamaz; bundan sonra konabilir."));
	} else {
		LOG(TR("• No folder without a titleId was found; most likely nothing changes and "
		       "there is no restart.",
		       "• titleId'si boş klasör bulunamadı; büyük ihtimalle hiçbir şey değişmez ve "
		       "yeniden başlatma olmaz."));
	}
	LOG(TR("• Folder positions, names and contents are not touched; only the titleId "
	       "field in app.db changes. Plugin and config.txt are not changed.",
	       "• Klasörlerin yerine, adına ve içeriğine dokunulmaz; yalnızca app.db'deki "
	       "titleId alanı değişir. Eklenti ve config.txt değişmez."));
	confirm_restart_note();
	return confirm_end();
}

static int confirm_uninstall(void)
{
	confirm_begin(TR("Uninstall", "Kaldır"));
	LOGW(TR("You are about to remove the plugin. If you confirm:",
	        "Eklentiyi kaldırmak üzeresin. Onaylarsan:"));
	if (S.db_ok && S.st.nested) {
		LOG(TR("• %d nested folder(s) will be moved to free slots on the home screen "
		       "(%d free). Their contents are kept.",
		       "• %d iç klasör ana menüdeki boş yerlere taşınacak (%d boş yer var). "
		       "İçindekiler korunur."), S.st.nested, S.st.free_slots);
		if (S.st.free_slots < S.st.nested)
			LOGE(TR("  Not enough free slots: nothing will be changed. Free some space first.",
			        "  Yeterli boş yer yok: hiçbir şey değiştirilmez. Önce yer aç."));
		LOG(TR("  Without the plugin, touching a nested folder freezes the home screen; "
		       "that is why they are moved out first.",
		       "  Eklenti olmadan iç içe klasöre dokunmak ana ekranı kilitler; bu yüzden "
		       "önce dışarı çıkarılırlar."));
	} else {
		LOG(TR("• There are no nested folders; folders are not moved.",
		       "• İç klasör yok; klasörlerin yeri değişmez."));
	}
	if (S.db_ok && S.st.our_tid)
		LOG(TR("• The titleIds the plugin gave to %d folder(s) (FLDRnnnnn) are cleared, as on "
		       "a stock Vita. Install / update gives them again if you reinstall.",
		       "• Eklentinin %d klasöre verdiği titleId'ler (FLDRnnnnn) temizlenecek; normal "
		       "Vita'daki gibi boş kalır. Tekrar kurarsan Kur / güncelle yeniden verir."),
		    S.st.our_tid);
	LOG(TR("• The nestfolder line is removed from config.txt and %s is deleted.",
	       "• config.txt içindeki nestfolder satırı silinecek, %s silinecek."), PLUGIN_DST);
	LOG(TR("• Backups and logs (%s) are kept.", "• Yedekler ve loglar (%s) silinmez."), DATA_DIR);
	LOG(TR("• Afterwards folders inside folders cannot be used. To install again "
	       "choose Install / update.",
	       "• Sonrasında klasör içinde klasör kullanılamaz. Tekrar kurmak için "
	       "Kur / güncelle'yi seç."));
	confirm_restart_note();
	return confirm_end();
}

static int confirm_restore(void)
{
	confirm_begin(TR("Restore first backup", "İlk yedeğe dön"));
	LOGE(TR("Warning: the home screen goes back to the state of the first backup.",
	        "Dikkat: ana ekran ilk yedekteki haline döner."));
	LOG(TR("• app.db is replaced with %s/app.db. Every home screen change made after "
	       "the first backup (folders, order, names) is lost.",
	       "• app.db, %s/app.db ile değiştirilecek. İlk yedekten sonra ana ekranda yapılan "
	       "tüm değişiklikler (klasörler, sıralama, adlar) kaybolur."), FIRST_DIR);
	LOG(TR("• Bubbles of apps installed after the first backup may disappear from the "
	       "home screen; the apps themselves are not deleted.",
	       "• İlk yedekten sonra kurulan uygulamaların balonları ana ekrandan kaybolabilir; "
	       "uygulamaların kendisi silinmez."));
	LOG(TR("• config.txt goes back to its first backup state (if the plugin line was "
	       "not there, it is removed). The plugin file is not deleted.",
	       "• config.txt ilk yedekteki haline döner (eklenti satırı orada yoksa silinir). "
	       "Eklenti dosyası silinmez."));
	LOG(TR("• The current app.db is kept as %s.", "• Şu anki app.db, %s olarak saklanır."), ORIG_DB);
	confirm_restart_note();
	return confirm_end();
}

static int confirm_exit(void)
{
	confirm_begin(TR("Exit", "Çıkış"));
	LOG(TR("The application will close. No file is changed.",
	       "Uygulama kapanacak. Hiçbir dosya değiştirilmez."));
	return confirm_end();
}

/* ===================================================================
 *  Menu
 * =================================================================== */
#define MENU_N 5
#define MENU_TOP 10

static const char *menu_item(int i)
{
	switch (i) {
	case 0: return TR("Install / update", "Kur / güncelle");
	case 1: return TR("Give titleIds to old folders", "Eski klasörlere titleId ver");
	case 2: return TR("Uninstall", "Kaldır");
	case 3: return TR("Restore first backup", "İlk yedeğe dön");
	default: return TR("Exit", "Çıkış");
	}
}

static void draw_menu(int sel)
{
	char buf[160];
	fill_rect(0, 0, SCR_W, SCR_H, C_BG);
	draw_title(NULL);

	snprintf(buf, sizeof(buf), TR("Plugin:       %s", "Eklenti:   %s"),
	         S.plugin == 1 ? TR("installed (up to date)", "kurulu (güncel)") :
	         S.plugin == 2 ? TR("installed (other version, can be updated)", "kurulu (farklı sürüm, güncellenebilir)") :
	                         TR("not installed", "kurulu değil"));
	draw_line(2, buf, S.plugin == 1 ? C_OK : C_WARN, C_BG);
	snprintf(buf, sizeof(buf), TR("Config:       %s, line %s", "Config:    %s, satır %s"), S.cfg,
	         S.cfg_line ? TR("present", "var") : TR("missing", "yok"));
	draw_line(3, buf, S.cfg_line ? C_OK : C_WARN, C_BG);
	if (S.db_ok) {
		snprintf(buf, sizeof(buf), TR("Folders:      %d folders, %d nested, %d without titleId",
		                              "Klasörler: %d klasör, %d iç klasör, %d titleId'siz"),
		         S.st.folders, S.st.nested, S.st.no_tid);
		draw_line(4, buf, S.st.no_tid ? C_WARN : C_TEXT, C_BG);
		if (S.st.nested_no_tid) {
			snprintf(buf, sizeof(buf), TR("              %d nested folder(s) hidden (no titleId)",
			                              "           %d iç klasör titleId'siz olduğu için görünmüyor"),
			         S.st.nested_no_tid);
			draw_line(5, buf, C_WARN, C_BG);
		}
	} else {
		snprintf(buf, sizeof(buf), TR("app.db:       %s", "app.db:    %s"), S.db_err);
		draw_line(4, buf, C_ERR, C_BG);
	}
	snprintf(buf, sizeof(buf), TR("First backup: %s", "İlk yedek: %s"), S.first ? FIRST_DIR : TR("none", "yok"));
	draw_line(6, buf, S.first ? C_TEXT : C_ERR, C_BG);
	draw_line(7, TR("3.60+ retail/devkit/testkit. Restarts after each operation.",
	                "3.60+ retail/devkit/testkit. Her işlemden sonra yeniden başlar."), C_DIM, C_BG);

	for (int i = 0; i < MENU_N; i++) {
		uint32_t bg = i == sel ? C_SEL_BG : C_BG;
		clear_row(MENU_TOP + i, bg);
		snprintf(buf, sizeof(buf), "%s %s", i == sel ? "→" : " ", menu_item(i));
		draw_text(2, MENU_TOP + i, buf, i == sel ? C_WHITE : C_TEXT, bg);
	}
	snprintf(buf, sizeof(buf), TR("↑↓: select   %s: choose", "↑↓: seç   %s: seç ve onaya geç"), K_OK);
	draw_help(buf);
}

static void fatal_screen(const char *l1, const char *l2)
{
	char help[48];
	fill_rect(0, 0, SCR_W, SCR_H, C_BG);
	draw_title(NULL);
	draw_line(3, l1, C_ERR, C_BG);
	if (l2) draw_line(5, l2, C_TEXT, C_BG);
	snprintf(help, sizeof(help), TR("%s / %s: exit", "%s / %s: çıkış"), K_OK, K_BACK);
	draw_help(help);
	wait_buttons(BTN_OK | BTN_BACK);
	sceKernelExitProcess(0);
}

/* Sistem dili ve Enter tusu */
static void read_system_params(void)
{
	SceAppUtilInitParam ip;
	SceAppUtilBootParam bp;
	memset(&ip, 0, sizeof(ip));
	memset(&bp, 0, sizeof(bp));
	sceAppUtilInit(&ip, &bp);
	int v = 0;
	if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &v) >= 0)
		g_tr = v == SCE_SYSTEM_PARAM_LANG_TURKISH;
	v = 0;
	if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &v) >= 0 &&
	    v == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
		BTN_OK = SCE_CTRL_CIRCLE; BTN_BACK = SCE_CTRL_CROSS;
		K_OK = "O"; K_BACK = "X";
	}
}

/* SceSqlite icin bellek fonksiyonlari */
static void *sq_malloc(int n) { return malloc(n); }
static void *sq_realloc(void *p, int n) { return realloc(p, n); }
static void sq_free(void *p) { free(p); }

int main(void)
{
	read_system_params();
	fb_init();
	fill_rect(0, 0, SCR_W, SCR_H, C_BG);
	draw_title(NULL);
	draw_line(2, TR("Preparing...", "Hazırlanıyor..."), C_TEXT, C_BG);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
	sceShellUtilInitEvents(0);

	if (sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE) < 0)
		fatal_screen(TR("Cannot load the SQLite module.", "SQLite modülü yüklenemedi."), NULL);
	static SceSqliteMallocMethods mm = {sq_malloc, sq_realloc, sq_free};
	sceSqliteConfigMallocMethods(&mm);
	sqlite3_initialize();
	if (nest_vfs_register() != SQLITE_OK)
		fatal_screen(TR("Cannot register the SQLite file layer.", "SQLite dosya katmanı kaydedilemedi."), NULL);
	db_set_vfs(NEST_VFS_NAME);

	if (!exists(LIVE_DB))
		fatal_screen(TR("Cannot read ur0:shell/db/app.db.", "ur0:shell/db/app.db okunamıyor."),
		             TR("'Enable Unsafe Homebrew' must be on in HENkaku settings.",
		                "HENkaku ayarlarında 'Enable Unsafe Homebrew' açık olmalı."));
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir(DATA_DIR, 0777);
	migrate_old_dirs();
	mkdirs();
	if (!exists(WORK_DIR))
		fatal_screen(TR("Cannot create the ux0:data/nestfolder folder.", "ux0:data/nestfolder klasörü oluşturulamadı."), NULL);
	int fb_r = first_backup();
	if (fb_r == -2)
		fatal_screen(TR("First backup failed: Shell is writing app.db right now.",
		                "İlk yedek alınamadı: Shell şu an app.db'ye yazıyor."),
		             TR("Wait a few seconds and open the app again.",
		                "Birkaç saniye bekleyip uygulamayı tekrar aç."));
	if (fb_r < 0)
		fatal_screen(TR("First backup failed; for safety nothing will be done.",
		                "İlk yedek alınamadı; güvenlik için hiçbir işlem yapılmayacak."),
		             TR("Check free space on ux0: and open the app again.",
		                "ux0: boş alanını kontrol et ve uygulamayı tekrar aç."));

	int sel = 0;
	refresh_status();
	draw_menu(sel);
	btn_prev = buttons_now();
	for (;;) {
		uint32_t p = wait_buttons(SCE_CTRL_UP | SCE_CTRL_DOWN | BTN_OK);
		if (p & SCE_CTRL_UP) sel = (sel + MENU_N - 1) % MENU_N;
		else if (p & SCE_CTRL_DOWN) sel = (sel + 1) % MENU_N;
		else if (p & BTN_OK) {
			switch (sel) {
			case 0: if (confirm_install()) op_install(); break;
			case 1: if (confirm_titleids()) op_titleids(); break;
			case 2: if (confirm_uninstall()) op_uninstall(); break;
			case 3: if (confirm_restore()) op_restore(); break;
			default: if (confirm_exit()) sceKernelExitProcess(0); break;
			}
			refresh_status();
			btn_prev = buttons_now();
		}
		draw_menu(sel);
	}
	return 0;
}
