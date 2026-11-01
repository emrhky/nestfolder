/*
 * nestfolder 1.0.3 - PS Vita (bilinen 3.60+ SceShell duzenleri) LiveArea ic ice klasor eklentisi
 *
 * Ozet:
 *  - Klasor icinde klasor acma (en fazla 2 seviye). Ic klasor acilirken
 *    Shell ust klasoru kapatip ogeleri yok ettigi icin, Shell'e ogenin
 *    eklentiye ait bir kopyasi verilir. Kopya GERCEK kaydi (+0x34) gosterir;
 *    kaydin referans sayaci (+0x38 kontrol blogu, sayac +4) bir artirilir.
 *    Her kopyanin kendi taklit kabi (+0x14) var; kabin +0x1c fonksiyonu
 *    acilistan once gercek kaptan alinan sayfa kimligini dondurur
 *    (ad degistirme kaydi SetIconInfoJob bunu kullanir).
 *    Kopyanin ad alani (+0x04, paf::wstring {ptr,len}) gercek ogenin metin
 *    bellegini gosteriyordu; gercek oge yok edilince bu bellek serbest
 *    kaliyor, ad degistirme Shell wstring atamasindan once ayni bellegi
 *    ikinci kez serbest birakiyordu (ara sira kilitlenme). 2.27: kopya,
 *    adin kendi kopyasini tasir.
 *  - Geri (O) ust klasore doner.
 *  - Birakma kontrolu ("oge klasor mu?"): ana menude her sey
 *    serbest (titleId'si bos klasor haric); bir klasorun icindeyken alt
 *    klasore uygulama birakilabilir, klasor birakilamaz.
 *  - Klasor olusturulurken titleId atanir. Kullanilan FLDR numaralari
 *    acilista app.db ham taranarak bulunur.
 *  - Klasor listesine titleId'si bos oge eklenmesi engellenir.
 *  - Klasor ikonu kucuk resmi (ReloadFolderIconCaptureJob):
 *    is, klasorun icindekileri app.db'den (SceLsdb) okuyup resmi ciziyor ve
 *    satirin reserved05 alanina yaziyor. Shell klasor icerigini app.db'ye
 *    klasor KAPANIRKEN yaziyor; klasor listesi kancasi (FL_ADD) sadece
 *    bellekteki listeyi degistiriyor. Bu yuzden ic klasor icin cizim, Shell
 *    kapanis isini kuyruga koyduktan SONRA istenir:
 *      - ic klasor kapanirken: orijinal kapanistan sonra kopya oge icin;
 *      - ust klasorun icinden alt klasore uygulama atilinca: oge kopyalanip
 *        bekletilir, ust klasor kapandiktan sonra cizilir.
 *    Ic klasorden ust klasore donulunce ust klasorun resmi de yenilenir.
 *    Sinir: Shell cizilen resmi bellekte tutar ve sadece ana menudeki
 *    ogeler icin yeniler; ic klasorun yeni resmi yeniden baslatinca gorunur.
 *    (2.20-2.25'te denenen onbellek yenileme yollari kaldirildi: biri ise
 *    yaramadi, onbellekten atma ise Shell'i kilitledi.)
 *
 * Acilis: sabit 20 sn yerine profilin Shell global yoneticisi ve onun
 * gorunum (+0x24) / kuyruk (+0x4) alanlari dolana kadar 100 ms'de bir
 * bekler, sonra 1 sn daha bekleyip kancalari takar (en fazla 60 sn).
 *
 * Log: her acilista yeni log.txt; onceki acilisin logu log.prev.txt.
 * Yedek: 1.0.0'dan itibaren eklenti app.db yedegi almaz; ilk yedegi kurulum
 * uygulamasi (installer/) bir kez alir.
 *
 * SceShell segment boyutlari ve sabit kod imzalariyla profil secilir; her
 * kanca noktasi secilen surumun beklenen koduyla karsilastirilir.
 * Ayar: ux0:data/nestfolder/disable, .../snapshot, .../delay20
 */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include "shell_profiles.h"

/* ===================================================================
 *  taiHEN
 * =================================================================== */
typedef uintptr_t tai_hook_ref_t;
struct _tai_hook_user { uintptr_t next; void *func; void *old; };
typedef struct {
	SceSize size;
	SceUID modid;
	uint32_t module_nid;
	char name[27];
	uintptr_t exports_start, exports_end, imports_start, imports_end;
} tai_module_info_t;
typedef struct {
	SceSize size;
	SceUID modid;
	int segidx;
	uint32_t offset;
	int thumb;
	const void *source;
	SceSize source_size;
} tai_offset_args_t;
int taiGetModuleInfo(const char *module, tai_module_info_t *info);
SceUID taiHookFunctionOffsetForUser(tai_hook_ref_t *p_hook, tai_offset_args_t *args);
int taiHookRelease(SceUID tai_uid, tai_hook_ref_t hook);

#define TAI_NEXT(ref) ({ \
	struct _tai_hook_user *cur_ = (struct _tai_hook_user *)(ref); \
	struct _tai_hook_user *nx_ = (struct _tai_hook_user *)cur_->next; \
	nx_ ? nx_->func : cur_->old; })

typedef uint32_t (*fn4_t)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*str_assign_t)(uint32_t str, const char *src, int len);   /* paf::string::assign */
static str_assign_t shell_str_assign;

/* ===================================================================
 *  LOG  (printf bicimli, satir sonu otomatik eklenir)
 * =================================================================== */
#define NEST_DATA_DIR "ux0:data/nestfolder"
#define NEST_LOG_PATH NEST_DATA_DIR "/log.txt"
#define NEST_LOG_MAX  512

static int nest_log_dir_ready = 0;

void nest_log(const char *fmt, ...)
{
	char buf[NEST_LOG_MAX];
	va_list ap;
	va_start(ap, fmt);
	int n = sceClibVsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';
	buf[n]   = '\0';

	if (!nest_log_dir_ready) {
		sceIoMkdir("ux0:data", 0777);
		sceIoMkdir(NEST_DATA_DIR, 0777);
		nest_log_dir_ready = 1;
	}
	SceUID fd = sceIoOpen(NEST_LOG_PATH,
	                      SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0) return;
	sceIoWrite(fd, buf, n);
	sceIoClose(fd);
}

/* ===================================================================
 *  Ayarlar / SceShell imzalari
 * =================================================================== */
#define DISABLE_PATH     NEST_DATA_DIR "/disable"

static const shell_profile_t *shell_profile;

static const uint8_t EXPECT_OPEN[16]   = {0x2d,0xe9,0xf0,0x41,0x43,0xf2,0x44,0x03,0xc8,0xf2,0x61,0x13,0x1b,0x68,0x0d,0x1c};
static const uint8_t EXPECT_CLOSE[16]  = {0x70,0xb5,0x43,0xf2,0x44,0x01,0xc8,0xf2,0x61,0x11,0x09,0x68,0xc4,0xb2,0xd1,0xf8};
static const uint8_t EXPECT_FL_ADD[16] = {0x2d,0xe9,0xf0,0x43,0x85,0xb0,0x47,0xf6,0x44,0x49,0xce,0xf2,0x02,0x09,0xd9,0xf8};
static const uint8_t EXPECT_FL_DEL[16] = {0x2d,0xe9,0xf0,0x41,0x84,0xb0,0x47,0xf6,0x44,0x48,0xce,0xf2,0x02,0x08,0xd8,0xf8};
static const uint8_t EXPECT_ICONCAPTURE[16] = {0x2d,0xe9,0xf0,0x41,0x86,0xb0,0x40,0xf6,0x44,0x08,0xce,0xf2,0x01,0x08,0xd8,0xf8};
static const uint8_t EXPECT_ISFOLDER[16] = {0x40,0x6b,0x20,0xb1,0x40,0x6a,0x05,0x28,0x01,0xd1,0x01,0x20,0x00,0xe0,0x00,0x20};
static const uint8_t EXPECT_MKFOLDERJOB[16] = {0x2d,0xe9,0xf0,0x4f,0x87,0xb0,0x06,0x91,0x10,0x99,0x04,0x91,0x00,0x25,0xdd,0xf8};
static const uint8_t EXPECT_STR_ASSIGN[16]  = {0x2d,0xe9,0xf0,0x41,0x06,0x1c,0x30,0x68,0x0d,0x1c,0x14,0x1c,0x88,0x42,0x2f,0xd0};
static const uint8_t EXPECT_WSTR_ASSIGN[16] = {0x70,0xb5,0x0c,0x1c,0x05,0x1c,0x84,0x42,0x2d,0xd0,0x4e,0xf6,0x58,0x71,0x28,0x68};
static const uint8_t EXPECT_RELOAD[16] = {0x2d,0xe9,0xf0,0x4f,0xad,0xf6,0xec,0x0d,0x4b,0xf6,0x34,0x7b,0xce,0xf2,0x01,0x0b};
static const uint8_t EXPECT_CAPFIN[16] = {0x30,0xb5,0x83,0xb0,0x4b,0xf6,0x34,0x75,0xce,0xf2,0x01,0x05,0x29,0x68,0x02,0x91};
static const uint8_t EXPECT_IMGGET[16] = {0x2d,0xe9,0xf0,0x43,0x99,0xb0,0x0f,0x1c,0x4b,0xf6,0x34,0x79,0xce,0xf2,0x01,0x09};
static const uint8_t EXPECT_CAP_CALL[12] = {0x09,0xa9,0x28,0x1c,0x00,0x22,0x00,0x23,0x05,0xf0,0xb9,0xf8};
static const uint8_t EXPECT_CAP_POS[6]   = {0xdc,0xf8,0x08,0x00,0x08,0x90};
static const uint8_t EXPECT_CAP_LIST[4]  = {0xdd,0xf8,0x44,0x80};
static const uint8_t EXPECT_FLDR_PARAM[18] = {0x44,0xf2,0xa1,0x50,0x4e,0xf6,0x98,0x36,0xc8,0xf2,0x12,0x10,0x1a,0x90,0xce,0xf2,0x00,0x06};

static uint32_t shell_seg1;
static tai_hook_ref_t ref_open, ref_close, ref_fladd, ref_fldel, ref_mkjob, ref_isfolder, ref_capture;
static tai_hook_ref_t ref_reload, ref_capfin, ref_imgget;
static SceUID uid_open = -1, uid_close = -1, uid_fladd = -1, uid_fldel = -1, uid_mkjob = -1, uid_isfolder = -1, uid_capture = -1;
static SceUID uid_reload = -1, uid_capfin = -1, uid_imgget = -1;
static uint32_t gate1_ret, gate2_ret;   /* GATE cagrilarinin donus adresleri (|1) */
static int snapshot_mode;               /* 1: eski kopya davranisi */

static int ptr_ok(uint32_t p) { return p >= 0x81000000u && p < 0xA0000000u && (p & 3) == 0; }
static uint32_t rd32(uint32_t a) { return ptr_ok(a) ? *(const uint32_t *)a : 0; }
static void cpy32(uint32_t *d, uint32_t src, unsigned n)
{
	for (unsigned i = 0; i < n / 4; i++) d[i] = rd32(src + i * 4);
}
static uint32_t open_folder_now(void)
{
	return shell_profile ? rd32(rd32(shell_seg1 + shell_profile->seg1_g) + 0xfc) : 0;
}

/* ===================================================================
 *  h slotlari
 *  En kotu durumda kullanilan slot: NEST_MAX (yigin) + 1 (mevcut) + 1 (yeni)
 * =================================================================== */
#define NEST_MAX 4
#define MAX_NEST_LEVEL 2   /* ust klasor + 1 ic klasor */
#define NSLOT 16
_Static_assert(NSLOT >= NEST_MAX + 2, "NSLOT yetersiz");

typedef struct { uint32_t obj, ctrl, rctrl; int used; } hslot_t;
static hslot_t hslots[NSLOT];
static int slot_rr, cur_slot = -1;
static int nest_stack[NEST_MAX], nest_depth, in_internal;

static int slot_find_free(void)
{
	for (int k = 0; k < NSLOT; k++) {
		int i = (slot_rr + k) % NSLOT;
		if (!hslots[i].used) return i;
	}
	return -1;
}
static int slot_take_at(int i, uint32_t h)
{
	if (i < 0 || i >= NSLOT || hslots[i].used) return -1;
	uint32_t obj = rd32(h), ctrl = rd32(h + 4);
	if (!obj) return -1;
	slot_rr = (i + 1) % NSLOT;
	if (!ptr_ok(ctrl + 4)) ctrl = 0;
	if (ctrl) __atomic_fetch_add((int *)(ctrl + 4), 1, __ATOMIC_SEQ_CST);
	hslots[i].obj = obj; hslots[i].ctrl = ctrl; hslots[i].rctrl = 0; hslots[i].used = 1;
	return i;
}
static int slot_take(uint32_t h) { return slot_take_at(slot_find_free(), h); }
/* Referansi birak, ama asla 1'in altina indirme: son referans bizdeyse
 * nesne silinmez (kucuk bir sizinti), Shell'in yikicisini bizim thread'imizden
 * tetiklememek icin. */
static void ref_drop(uint32_t ctrl)
{
	if (!ctrl) return;
	int *p = (int *)(ctrl + 4);
	int v = __atomic_load_n(p, __ATOMIC_SEQ_CST);
	while (v > 1 && !__atomic_compare_exchange_n(p, &v, v - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
}
static void slot_release(int i)
{
	if (i < 0 || i >= NSLOT || !hslots[i].used) return;
	ref_drop(hslots[i].ctrl);
	ref_drop(hslots[i].rctrl);
	hslots[i].rctrl = 0;
	hslots[i].used = 0;
}
static void nest_clear(void)
{
	while (nest_depth > 0) slot_release(nest_stack[--nest_depth]);
	slot_release(cur_slot);
	cur_slot = -1;
}

/* ===================================================================
 *  fake item: SLOT basina tampon (derinlik basina degil)
 * =================================================================== */
static uint32_t fake_item[NSLOT][0x48 / 4];
static uint32_t fake_ctrl[NSLOT][4];
static uint32_t snap_icon[NSLOT][0x60 / 4];
static uint32_t snap_fi[NSLOT][0x40 / 4];
static uint32_t fake_cont[NSLOT][64];   /* slot basina taklit kap */
static uint32_t fake_cont_vt[64];       /* ortak vtable */
#define CONT_X_WORD (0x98 / 4)          /* kap+0x98: sayfa kimligi */

static void fake_deleter(uint32_t obj) { (void)obj; }
static uint32_t nop_fn(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)a; (void)b; (void)c; (void)d; return 0; }
/* vtable[7] (+0x1c): saklanan sayfa kimligini dondur */
static uint32_t cont_get_x(uint32_t cont)
{ return ((const uint32_t *)cont)[CONT_X_WORD]; }

/* Gercek ogenin kabindan sayfa kimligini Shell'in kendi fonksiyonuyla al.
 * Ic klasor acilisinda, Shell ust klasoru kapatip ogeleri yok etmeden ONCE
 * cagrilir. */
static uint32_t shell_seg0_lo, shell_seg0_hi;
typedef uint32_t (*fn1_t)(uint32_t);
static uint32_t real_cont_x(uint32_t item)
{
	uint32_t cont = rd32(item + 0x14);
	if (!cont) return 0;
	uint32_t vt = rd32(cont), f7 = rd32(vt + 0x1c);
	uint32_t x = 0;
	if (f7 >= shell_seg0_lo && f7 < shell_seg0_hi)
		x = ((fn1_t)f7)(cont);
	return x;
}

/* Bekleyen cizim: ust klasorun icinden alt klasore uygulama atilinca alt
 * klasor ogesinin kopyasi; ust klasor kapanana kadar bekler. */
#define NPEND 4
static uint32_t pend_item[NPEND][0x48 / 4];
static uint32_t pend_ctrl[NPEND][4];
static uint32_t pend_cont[NPEND][64];
static uint32_t pend_rec[NPEND], pend_rctrl[NPEND];
static uint32_t pend_late[NPEND];   /* bir sonraki bosaltmada birakilacak referanslar */
static int pend_used[NPEND];

/* paf::wstring: {ptr, len}; bos isaretci Shell kodundan okunur. */
typedef void (*wstr_assign_t)(uint32_t dst, uint32_t src);
static wstr_assign_t shell_wstr_assign;
static uint32_t paf_wempty;

/* Kopyanin ad alanina gercek ogenin adinin KENDI kopyasini koy. prev: bu
 * yuvanin onceki kullanimindan kalan, bize ait metin (ya da 0). */
static void fake_own_title(uint32_t *fitem, uint32_t prev_ptr, uint32_t prev_len, uint32_t real)
{
	if (!paf_wempty) { fitem[1] = 0; fitem[2] = 0; return; }
	fitem[1] = prev_ptr ? prev_ptr : paf_wempty;
	fitem[2] = prev_ptr ? prev_len : 0;
	if (shell_wstr_assign && rd32(real + 4))
		shell_wstr_assign((uint32_t)fitem + 4, real + 4);   /* eskisini serbest birakir */
	else if (!shell_wstr_assign) { fitem[1] = paf_wempty; fitem[2] = 0; }
}

static uint32_t build_fake(int k, uint32_t item)
{
	for (int i = 0; i < 64; i++) fake_cont_vt[i] = (uint32_t)nop_fn;
	fake_cont_vt[0x1c / 4] = (uint32_t)cont_get_x;
	for (int i = 0; i < 64; i++) fake_cont[k][i] = 0;
	fake_cont[k][0] = (uint32_t)fake_cont_vt;
	fake_cont[k][1] = 1;
	fake_cont[k][CONT_X_WORD] = real_cont_x(item);
	uint32_t ic = rd32(item + 0x34);
	uint32_t prev_ptr = fake_item[k][1], prev_len = fake_item[k][2];
	if (prev_ptr == paf_wempty) prev_ptr = 0;
	cpy32(fake_item[k], item, sizeof(fake_item[k]));
	fake_own_title(fake_item[k], prev_ptr, prev_len, item);
	if (snapshot_mode) {
		uint32_t fi = rd32(ic + 0x4c);
		if (ic) cpy32(snap_icon[k], ic, sizeof(snap_icon[k]));
		if (fi) {
			cpy32(snap_fi[k], fi, sizeof(snap_fi[k]));
			snap_icon[k][0x4c / 4] = (uint32_t)snap_fi[k];
		}
	}
	uint32_t fk = (uint32_t)fake_item[k];
	fake_item[k][0x10 / 4] = 0;
	fake_item[k][0x14 / 4] = (uint32_t)fake_cont[k];
	fake_item[k][0x30 / 4] = 0;
	/* +0x34/+0x38: kayit ve kontrol blogu. 2.8: gercek kayit (+0x38 zaten
	 * ogeden kopyalandi); eski davranista statik kopya. */
	if (snapshot_mode) fake_item[k][0x34 / 4] = (uint32_t)snap_icon[k];
	else               fake_item[k][0x34 / 4] = ic;
	fake_ctrl[k][0] = fk;
	fake_ctrl[k][1] = 0x40000000;
	fake_ctrl[k][2] = 0x40000000;
	fake_ctrl[k][3] = (uint32_t)fake_deleter;
	fake_item[k][0x40 / 4] = fk;
	fake_item[k][0x44 / 4] = (uint32_t)fake_ctrl[k];
	return fk;
}

/* Kopya ogeyi verilen tamponlara kur (bekleyen cizim icin). Kopya gercek
 * kaydi gosterir ve kaydin referansini artirir. */
static void pend_add(uint32_t item)
{
	uint32_t rec = rd32(item + 0x34), rc = rd32(item + 0x38);
	if (!rec || !ptr_ok(rc + 4)) return;
	for (int i = 0; i < NPEND; i++)
		if (pend_used[i] && pend_rec[i] == rec) return;   /* zaten bekliyor */
	int k = -1;
	for (int i = 0; i < NPEND; i++) if (!pend_used[i]) { k = i; break; }
	if (k < 0) return;
	for (int i = 0; i < 64; i++) fake_cont_vt[i] = (uint32_t)nop_fn;
	fake_cont_vt[0x1c / 4] = (uint32_t)cont_get_x;
	for (int i = 0; i < 64; i++) pend_cont[k][i] = 0;
	pend_cont[k][0] = (uint32_t)fake_cont_vt;
	pend_cont[k][1] = 1;
	pend_cont[k][CONT_X_WORD] = real_cont_x(item);
	cpy32(pend_item[k], item, sizeof(pend_item[k]));
	pend_item[k][1] = paf_wempty; pend_item[k][2] = 0;   /* gercek ogenin metnini paylasma */
	uint32_t fk = (uint32_t)pend_item[k];
	pend_item[k][0x10 / 4] = 0;
	pend_item[k][0x14 / 4] = (uint32_t)pend_cont[k];
	pend_item[k][0x30 / 4] = 0;
	pend_ctrl[k][0] = fk;
	pend_ctrl[k][1] = 0x40000000;
	pend_ctrl[k][2] = 0x40000000;
	pend_ctrl[k][3] = (uint32_t)fake_deleter;
	pend_item[k][0x40 / 4] = fk;
	pend_item[k][0x44 / 4] = (uint32_t)pend_ctrl[k];
	__atomic_fetch_add((int *)(rc + 4), 1, __ATOMIC_SEQ_CST);
	pend_rec[k] = rec; pend_rctrl[k] = rc; pend_used[k] = 1;
}

/* ===================================================================
 *  Kancalar
 * =================================================================== */
static int is_fake_item(uint32_t p);
static void request_capture(uint32_t item);
static uint32_t open_folder_item(void) { uint32_t fc = open_folder_now(); return fc ? rd32(fc + 0x68) : 0; }

/* Ust klasor kapandi: bekleyen cizimleri iste ve kayit referanslarini birak */
static void pend_flush(void)
{
	/* Onceki turun referanslari: o cizim isleri coktan bitti */
	for (int k = 0; k < NPEND; k++) { ref_drop(pend_late[k]); pend_late[k] = 0; }
	for (int k = 0; k < NPEND; k++) {
		if (!pend_used[k]) continue;
		request_capture((uint32_t)pend_item[k]);
		/* Kayit referansini is calisirken tutmaya devam et */
		pend_late[k] = pend_rctrl[k];
		pend_used[k] = 0;
	}
}
static uint32_t open_hook(uint32_t h, uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t obj = rd32(h);
	uint32_t fc = open_folder_now();
	if (in_internal || !obj)
		return ((fn4_t)TAI_NEXT(ref_open))(h, a, b, c);
	uint32_t *pview = (uint32_t *)(shell_seg1 + shell_profile->seg1_view);
	uint32_t r;
	if (fc != 0 && (cur_slot < 0 || hslots[cur_slot].obj != obj)) {
		/* 2 seviye siniri: zaten bir ic klasordeysek (nest_depth>=1) daha
		 * derine inme, dokunusu yoksay. */
		if (nest_depth >= MAX_NEST_LEVEL - 1)
			return 0;
		/* Klasor icinde klasor. Slot alinamazsa orijinal acilisa DUSME:
		 * o yol Shell'i cokerten yol. Acilisi yoksay. */
		int s = slot_find_free();
		if (s < 0) return 0;
		uint32_t fk = build_fake(s, obj);
		if (slot_take_at(s, fk + 0x40) < 0) return 0;
		if (!snapshot_mode) {
			/* Shell asagida acik klasoru kapatip ogeleri yok edecek; kayit
			 * onlarla birlikte silinmesin diye referans al. */
			uint32_t rc = rd32(obj + 0x38);
			if (rd32(obj + 0x34) && ptr_ok(rc + 4)) {
				__atomic_fetch_add((int *)(rc + 4), 1, __ATOMIC_SEQ_CST);
				hslots[s].rctrl = rc;
			}
		}
		if (nest_depth >= NEST_MAX) {
			slot_release(nest_stack[0]);
			for (int i = 1; i < NEST_MAX; i++) nest_stack[i - 1] = nest_stack[i];
			nest_depth--;
		}
		nest_stack[nest_depth++] = cur_slot;
		cur_slot = s;
		in_internal = 1;
		((fn4_t)TAI_NEXT(ref_close))(0, 0, 0, 0);
		uint32_t pv = *pview;
		if (pv && ptr_ok(rd32(pv) + 4)) {
			*pview = 0;
			((void (*)(uint32_t))rd32(rd32(pv) + 4))(pv);
		}
		r = ((fn4_t)TAI_NEXT(ref_open))((uint32_t)&hslots[s], a, b, c);
		in_internal = 0;
	} else if (fc == 0) {
		nest_clear();
		r = ((fn4_t)TAI_NEXT(ref_open))(h, a, b, c);
		cur_slot = slot_take(h);
	} else {
		r = ((fn4_t)TAI_NEXT(ref_open))(h, a, b, c);
	}
	return r;
}

static uint32_t close_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	uint32_t closing = (!in_internal && nest_depth > 0 && cur_slot >= 0)
	                   ? hslots[cur_slot].obj : 0;
	uint32_t r = ((fn4_t)TAI_NEXT(ref_close))(a0, a1, a2, a3);
	if (in_internal || open_folder_now() != 0) return r;
	/* Ic klasor kapandi. Shell icerigi app.db'ye yazan isi kapanista kuyruga
	 * koydu; cizimi simdi istersek ondan sonra calisir ve yeni icerigi okur. */
	if (closing && is_fake_item(closing)) request_capture(closing);
	if (nest_depth > 0 && (a0 & 0xff) == 1 && nest_stack[nest_depth - 1] >= 0) {
		int parent = nest_stack[--nest_depth];
		slot_release(cur_slot);
		cur_slot = parent;
		in_internal = 1;
		((fn4_t)TAI_NEXT(ref_open))((uint32_t)&hslots[parent], 0, 0, 0);
		in_internal = 0;
		/* Ust klasorun resmi ic klasorun resmini iceriyor: onu da yenile.
		 * Is kuyrugu sirali, ic klasorun isinden sonra calisir. */
		uint32_t po = hslots[parent].obj;
		if (po && !is_fake_item(po)) request_capture(po);
	} else {
		nest_clear();
		pend_flush();   /* ana menuye donuldu: bekleyen cizimler */
	}
	return r;
}

/* Klasor listesine titleId'si bos oge eklenmesini/cikarilmasini engelle */
static uint32_t fladd_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	if (a1 == 0) return 0;
	uint32_t r = ((fn4_t)TAI_NEXT(ref_fladd))(a0, a1, a2, a3);
	/* Klasor icindeyken alt klasore uygulama atildi: normal Vita'da bu durum
	 * olmadigi icin Shell alt klasorun ikonunu cizmez. Icerik app.db'ye ust
	 * klasor kapanirken yazilacak; ogeyi kopyalayip o ana kadar beklet. */
	if (!in_internal && open_folder_now() != 0 && !is_fake_item(a0) &&
	    a0 != open_folder_item())
		pend_add(a0);
	return r;
}
static uint32_t fldel_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{ if (a1 == 0) return 0; return ((fn4_t)TAI_NEXT(ref_fldel))(a0, a1, a2, a3); }

/* "Oge klasor mu?". Orijinali hic cagrilmaz: icindeki
 * cbz, taiHEN'in kopyaladigi trampolinde bozulur. Mantik birebir ayni.
 * Birakma fonksiyonundaki iki kontrol noktasindan (GATE1/GATE2) gelirse
 * "item" suruklenen ogedir ve 1 donusu birakmayi reddeder:
 *   ana menu (klasor acik degil)   -> 0: serbest; ancak suruklenen
 *                                      titleId'si bos bir klasorse 1
 *                                      (listeye eklenemez, gorunmez olurdu)
 *   bir klasorun icindeyiz           -> suruklenen klasorse 1, uygulamaysa 0 */
static uint32_t isfolder_hook(uint32_t item)
{
	uint32_t lr = (uint32_t)__builtin_return_address(0) | 1u;
	uint32_t rec = *(const uint32_t *)(item + 0x34);
	uint32_t is_folder = (rec && *(const uint32_t *)(rec + 0x24) == 5) ? 1u : 0u;
	if (lr == gate1_ret || lr == gate2_ret) {
		/* Burada "item" suruklenen oge; donus 1 = birakmayi reddet. */
		if (open_folder_now() == 0) {
			/* ana menu: serbest; titleId'si bos klasor baska klasore giremez */
			if (is_folder && *(const uint32_t *)(rec + 0x2c) == 0) return 1u;
			return 0u;
		}
		return is_folder;                        /* klasor icinde: sadece uygulama */
	}
	return is_folder;
}

/* Klasor ikonu cizim istegi. Is, (kap+0x1c = sayfa no,
 * oge+0x20 = pos) satirinin reserved05 alanina resmi yazar; ad degistirme de
 * ayni satiri (kayit+0x08 = pos) kullanir. Kopya ogeler de buradan gecer. */
static int is_fake_item(uint32_t p)
{
	uint32_t lo = (uint32_t)fake_item, hi = lo + sizeof(fake_item);
	uint32_t plo = (uint32_t)pend_item, phi = plo + sizeof(pend_item);
	return (p >= lo && p < hi) || (p >= plo && p < phi);
}
static uint32_t capture_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
	return ((fn4_t)TAI_NEXT(ref_capture))(a0, a1, a2, a3);
}
/* ===================================================================
 *  Ic klasor ikonunu aninda yenileme (1.0.1)
 *
 *  Shell klasor resmini "<icerik sayfa no>:<rowid>" anahtariyla bir resim
 *  onbelleginde tutar. Cizim isi yeni resmi app.db'ye yazar; bitiste
 *  bitis fonksiyonu ogeyi YALNIZCA ana menu sayfalarinda arar ve
 *  bulursa reload(oge, 1) ile onbellekten atip yeniden yukler. Ic klasorun
 *  ogesi acik klasorun yuvalarinda (fc+0x08 + 8*i, i<10; sayfalarla ayni
 *  duzen, 0xD3996) durdugu icin bulunamaz ve eski resim onbellekte kalir.
 *
 *  Eklenti, cizimini istedigi her oge icin rowid'i izler:
 *   1 = cizim istendi, 2 = cizim bitti (yenilenmeli).
 *  Ic klasorun ogesi, ust klasor acilirken kurucusunda (0xCFB92) resmini
 *  oge kurucusundan reload(oge, 0) ile onbellekten alir; durum 2 ise hemen
 *  reload(oge, 1) ile onbellekten atilip yeniden yuklenir. Acik klasorun
 *  fc+0x08 yuvalari bu anda bos oldugu icin bitiste arama yapilmaz.
 *  Shell bir ogeyi (onceden resmi varken) zorla yenilerse izleme biter.
 * =================================================================== */
#define NWATCH 8
typedef struct { uint32_t lo, hi; int state; } watch_t;
static watch_t watch[NWATCH];
static int watch_rr, in_reload;

static watch_t *watch_find(uint32_t lo, uint32_t hi)
{
	if (!lo && !hi) return 0;
	for (int i = 0; i < NWATCH; i++)
		if (watch[i].state && watch[i].lo == lo && watch[i].hi == hi) return &watch[i];
	return 0;
}
static void watch_request(uint32_t rec)
{
	if (uid_reload < 0 || uid_capfin < 0 || !rec) return;
	uint32_t lo = rd32(rec), hi = rd32(rec + 4);
	if (!lo && !hi) return;
	watch_t *w = watch_find(lo, hi);
	if (!w) {
		w = &watch[watch_rr];
		watch_rr = (watch_rr + 1) % NWATCH;
		w->lo = lo; w->hi = hi;
	}
	w->state = 1;
}

static int subicon_on;
static void request_capture(uint32_t item);

static void reload_now(uint32_t item, watch_t *w)
{
	in_reload = 1;
	((fn4_t)TAI_NEXT(ref_reload))(item, 1, 0, 0);
	in_reload = 0;
	w->state = 0;
	/* Ust klasorun kucuk resmi ic klasorun resmini iceriyor: o cizilirken ic
	 * klasorun eski resmi onbellekte olabilirdi. Simdi bir kez daha ciz. */
	if (subicon_on) {
		uint32_t parent = open_folder_item();
		if (parent && parent != item) request_capture(parent);
	}
}

/* Shell reload(oge, zorla) */
static uint32_t reload_hook(uint32_t item, uint32_t force, uint32_t a2, uint32_t a3)
{
	uint32_t lr = (uint32_t)__builtin_return_address(0);
	uint32_t old = item ? rd32(item + 0x30) : 0;
	uint32_t r = ((fn4_t)TAI_NEXT(ref_reload))(item, force, a2, a3);
	if (in_reload || !item) return r;
	uint32_t rec = rd32(item + 0x34);
	if (!rec) return r;
	watch_t *w = watch_find(rd32(rec), rd32(rec + 4));
	if (!w) return r;
	if (w->state != 2) return r;
	/* Shell zorla yeniledi VE oncesinde resim vardi: kayit onbellekten atildi */
	if ((force & 0xff) && old) { w->state = 0; return r; }
	/* Ilk yukleme (zorla olsa da onceki resim yoktu) onbellekteki eski resmi
	 * verdi: bir kez zorla yenile */
	/* Klasor acilirken oge kurucusu (0xCFB92) resmi 0xCFD50'den yukler; bu
	 * anda widget ve kendine weak_ptr (+0x40) henuz yok. Reload widget
	 * yoksa ekran guncellemesini atlar. Gecerli oge: kurucudan gelen ya da
	 * tam kurulmus (+0x40 == oge). */
	uint32_t from = (lr & ~1u) - shell_seg0_lo;
	int valid = from == shell_profile->reload_from_ctor || rd32(item + 0x40) == item;
	if (!is_fake_item(item) && valid && rd32(item + 0x30))
		reload_now(item, w);
	return r;
}

/* ReloadFolderIconCaptureJob bitisi */
static uint32_t capfin_hook(uint32_t job, uint32_t a1, uint32_t a2, uint32_t a3)
{
	uint32_t lo = rd32(job + 0x30), hi = rd32(job + 0x34);
	watch_t *w = watch_find(lo, hi);
	if (w && w->state == 1) w->state = 2;                 /* ana menudeyse Shell yeniler */
	/* Ana menudeki ogeyi Shell kendisi yeniler (reload(oge, 1)); ic klasorun
	 * ogesi ise klasor acilirken kurulurken yenilenir (reload_hook). */
	return ((fn4_t)TAI_NEXT(ref_capfin))(job, a1, a2, a3);
}

/* ===================================================================
 *  Ust klasorun kucuk resminde ic klasorun resmi (1.0.1)
 *
 *  Cizim isi alt ogelerin resimlerini once onbellege alir:
 *  her alt kayit icin iconPath (+0x18) yolunu imgget(yol, &resim, 0, 0, 0)
 *  ile ister, {pos, resim} listesine koyar, sonra yuklenmeleri bekleyip
 *  cizer. Klasor kayitlarinin iconPath'i yok; klasor resmi
 *  "<icerik sayfa no>:<rowid>" anahtari ve ozel yukleyiciyle gelir
 *  (reload ve klasor yukleyicisi). Bu cagri noktasinda alt kayit bir klasorse
 *  istek ayni anahtar ve parametrelerle yapilir; resim onbellekte ana menu ve
 *  klasor icindeki ogeyle ortaktir.
 *  Alt kayit, cagirinin yigindan bulunur: [sp+0x20] = pos, [sp+0x44] =
 *  liste -> {veri, adet}, veri[i] = {kayit, kontrol}.
 *  Kapatmak icin: ux0:data/nestfolder/nosubicon
 * =================================================================== */
typedef uint32_t (*fn5_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t fldr_loader, paf_sempty;

static uint32_t cap_child_record(uint32_t csp)
{
	uint32_t pos = rd32(csp + 0x20), list = rd32(csp + 0x44);
	uint32_t vec = rd32(list), data = rd32(vec), n = rd32(vec + 4);
	if (!data || n == 0 || n > 64) return 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t rec = rd32(data + i * 8);
		if (rec && rd32(rec + 8) == pos) return rec;
	}
	return 0;
}

static uint32_t imgget_hook(uint32_t key, uint32_t out, uint32_t param, uint32_t a3, uint32_t s0)
{
	uint32_t lr = ((uint32_t)__builtin_return_address(0) & ~1u) - shell_seg0_lo;
	if (lr != shell_profile->cap_child_ret || param != 0 || !subicon_on)
		return ((fn5_t)TAI_NEXT(ref_imgget))(key, out, param, a3, s0);

	uint32_t rec = cap_child_record((uint32_t)&s0);   /* &s0 = cagirinin sp'si */
	uint32_t fi = rec ? rd32(rec + 0x4c) : 0;
	if (!rec || rd32(rec + 0x24) != 5 || !ptr_ok(fi) || rd32(rec + 4) != 0) {
		return ((fn5_t)TAI_NEXT(ref_imgget))(key, out, param, a3, s0);
	}

	char buf[32];
	int len = sceClibSnprintf(buf, sizeof(buf), "%d:%u", (int)rd32(fi + 0x14), (unsigned)rd32(rec));
	uint32_t str[4] = {paf_sempty, 0, 0, 0};
	shell_str_assign((uint32_t)str, buf, len);

	/* Reload fonksiyonunun klasor parametreleri (sp+0x50, 0x54 bayt) */
	uint32_t pr[0x54 / 4];
	sceClibMemset(pr, 0, sizeof(pr));
	uint8_t *pb = (uint8_t *)pr;
	pb[0x00] = 1; pb[0x01] = 1; pr[0x04 / 4] = 0xFFFFFFFFu; pb[0x08] = 1;
	pr[0x18 / 4] = fldr_loader; pr[0x38 / 4] = 1; pr[0x40 / 4] = 0xFFFFFFFFu;

	uint32_t r = ((fn5_t)TAI_NEXT(ref_imgget))((uint32_t)str, out, (uint32_t)pr, 0, 0);
	if (str[0] != paf_sempty && str[0])
		shell_str_assign((uint32_t)str, NULL, 0);
	return r;
}

static void request_capture(uint32_t item)
{
	if (uid_capture < 0 || !item) return;
	watch_request(rd32(item + 0x34));
	capture_hook(item, 0, 0, 0);
}

static uint32_t thumb_imm16(const uint8_t *p)
{
	uint16_t h1 = p[0] | (p[1] << 8), h2 = p[2] | (p[3] << 8);
	return ((h1 & 0xF) << 12) | (((h1 >> 10) & 1) << 11) | (((h2 >> 12) & 7) << 8) | (h2 & 0xFF);
}
static int thumb_bl_targets(const uint8_t *code, const uint8_t *target)
{
	uint16_t h1 = code[0] | (code[1] << 8);
	uint16_t h2 = code[2] | (code[3] << 8);
	if ((h1 & 0xF800u) != 0xF000u || (h2 & 0xD000u) != 0xD000u)
		return 0;
	uint32_t s = (h1 >> 10) & 1u;
	uint32_t j1 = (h2 >> 13) & 1u;
	uint32_t j2 = (h2 >> 11) & 1u;
	uint32_t i1 = (~(j1 ^ s)) & 1u;
	uint32_t i2 = (~(j2 ^ s)) & 1u;
	uint32_t imm25 = (s << 24) | (i1 << 23) | (i2 << 22) |
	                 ((h1 & 0x03FFu) << 12) | ((h2 & 0x07FFu) << 1);
	int32_t displacement = (int32_t)(imm25 << 7) >> 7;
	uint32_t destination = (uint32_t)code + 4u + (uint32_t)displacement;
	return destination == (uint32_t)target;
}
static int code_matches(const uint8_t *code, const uint8_t *expect, int len)
{
	int i = 0;
	while (i + 1 < len) {
		uint16_t hw = expect[i] | (expect[i + 1] << 8);
		int wide = (hw >> 11) == 0x1D || (hw >> 11) == 0x1E || (hw >> 11) == 0x1F;
		if (wide && ((hw & 0xFBF0) == 0xF240 || (hw & 0xFBF0) == 0xF2C0)) {
			if (i + 3 >= len) break;
			uint16_t c1 = code[i] | (code[i + 1] << 8);
			uint16_t e2 = expect[i + 2] | (expect[i + 3] << 8);
			uint16_t c2 = code[i + 2] | (code[i + 3] << 8);
			if ((c1 & 0xFBF0) != (hw & 0xFBF0) || (c2 & 0x0F00) != (e2 & 0x0F00))
				return 0;
			i += 4; continue;
		}
		int n = wide ? 4 : 2;
		for (int k = 0; k < n && i + k < len; k++)
			if (code[i + k] != expect[i + k]) return 0;
		i += n;
	}
	return 1;
}
static SceUID hook_at(tai_hook_ref_t *ref, SceUID modid, uint32_t off, const void *fn)
{
	tai_offset_args_t a;
	a.size = sizeof(a); a.modid = modid; a.segidx = 0; a.offset = off;
	a.thumb = 1; a.source = fn; a.source_size = 0;
	return taiHookFunctionOffsetForUser(ref, &a);
}
static int bytes_equal(const uint8_t *a, const uint8_t *b, int n)
{ for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

static int shell_profile_matches(const uint8_t *seg0, const shell_profile_t *p)
{
	return code_matches(seg0 + p->folder_open, EXPECT_OPEN, sizeof(EXPECT_OPEN)) &&
	       code_matches(seg0 + p->folder_close, EXPECT_CLOSE, sizeof(EXPECT_CLOSE)) &&
	       code_matches(seg0 + p->isfolder, EXPECT_ISFOLDER, sizeof(EXPECT_ISFOLDER)) &&
	       code_matches(seg0 + p->str_assign, EXPECT_STR_ASSIGN, sizeof(EXPECT_STR_ASSIGN)) &&
	       code_matches(seg0 + p->wstr_assign, EXPECT_WSTR_ASSIGN, sizeof(EXPECT_WSTR_ASSIGN));
}

static const shell_profile_t *shell_profile_find(const uint8_t *seg0,
		uint32_t seg0_size, uint32_t seg1_size)
{
	const shell_profile_t *found = NULL;
	for (unsigned i = 0; i < sizeof(shell_profiles) / sizeof(shell_profiles[0]); i++) {
		const shell_profile_t *p = &shell_profiles[i];
		if (p->seg0_size != seg0_size || p->seg1_size != seg1_size ||
		    !shell_profile_matches(seg0, p))
			continue;
		if (found) return NULL;
		found = p;
	}
	return found;
}

/* ===================================================================
 *  Klasor olusturulurken titleId atama
 * =================================================================== */
#define APP_DB_PATH "ur0:shell/db/app.db"
#define FLDR_MAX    100000

typedef uint32_t (*fn8_t)(uint32_t, uint32_t, uint32_t, uint32_t,
                          uint32_t, uint32_t, uint32_t, uint32_t);
static uint8_t fldr_used[FLDR_MAX / 8];
static int fldr_next = 1;

static void fldr_mark(int n)
{
	if (n > 0 && n < FLDR_MAX) fldr_used[n / 8] |= (uint8_t)(1u << (n % 8));
}

/* app.db'yi ham oku, icindeki tum "FLDRddddd" degerlerini isaretle.
 * Silinmis sayfalardaki eski degerler de sayilir: bu sadece daha temkinli. */
static void fldr_scan_appdb(void)
{
	static uint8_t buf[4096 + 8];
	SceUID fd = sceIoOpen(APP_DB_PATH, SCE_O_RDONLY, 0);
	if (fd < 0) { nest_log("[tid] cannot open app.db: 0x%08X", (unsigned)fd); return; }
	int keep = 0;
	for (;;) {
		int n = sceIoRead(fd, buf + keep, 4096);
		if (n <= 0) break;
		int len = keep + n;
		for (int i = 0; i + 9 <= len; i++) {
			if (buf[i] != 'F' || buf[i+1] != 'L' || buf[i+2] != 'D' || buf[i+3] != 'R') continue;
			int v = 0, ok = 1;
			for (int k = 4; k < 9; k++) {
				if (buf[i+k] < '0' || buf[i+k] > '9') { ok = 0; break; }
				v = v * 10 + (buf[i+k] - '0');
			}
			if (ok) fldr_mark(v);
		}
		/* sinira denk gelen eslesme icin son 8 bayti tasi */
		keep = len < 8 ? len : 8;
		for (int i = 0; i < keep; i++) buf[i] = buf[len - keep + i];
	}
	sceIoClose(fd);
}

static int fldr_alloc(char out[10])
{
	while (fldr_next < FLDR_MAX && (fldr_used[fldr_next / 8] & (1u << (fldr_next % 8))))
		fldr_next++;
	if (fldr_next >= FLDR_MAX) return -1;
	int v = fldr_next;
	fldr_mark(v);
	out[0]='F'; out[1]='L'; out[2]='D'; out[3]='R';
	for (int p = 8; p >= 4; p--) { out[p] = (char)('0' + (v % 10)); v /= 10; }
	out[9] = 0;
	return 0;
}

/* r0..r3 + 4 yigin argumani. s0 = &shared_ptr<kayit> */
static uint32_t mkjob_hook(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                           uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3)
{
	uint32_t rec = rd32(s0);
	if (rec && shell_str_assign &&
	    rd32(rec + 0x24) == 5 && rd32(rec + 0x34) == 7 &&   /* klasor kaydi */
	    rd32(rec + 0x2c) == 0) {                             /* titleId bos */
		char tid[10];
		if (fldr_alloc(tid) == 0) {
			shell_str_assign(rec + 0x28, tid, 9);
			nest_log("[tid] new folder -> %s", tid);
		} else {
			nest_log("[tid] no free FLDR number left");
		}
	}
	return ((fn8_t)TAI_NEXT(ref_mkjob))(r0, r1, r2, r3, s0, s1, s2, s3);
}

/* ===================================================================
 *  setup / module_start / module_stop
 * =================================================================== */
static void install_hooks(void)
{
	tai_module_info_t ti;
	ti.size = sizeof(ti);
	if (taiGetModuleInfo("SceShell", &ti) < 0) {
		nest_log("[nest] taiGetModuleInfo(SceShell) FAIL");
		return;
	}
	SceKernelModuleInfo mi;
	sceClibMemset(&mi, 0, sizeof(mi));
	mi.size = sizeof(mi);
	if (sceKernelGetModuleInfo(ti.modid, &mi) < 0) {
		nest_log("[nest] sceKernelGetModuleInfo FAIL");
		return;
	}
	const uint8_t *seg0 = mi.segments[0].vaddr;
	shell_profile = shell_profile_find(seg0, mi.segments[0].memsz, mi.segments[1].memsz);
	if (!shell_profile) {
		nest_log("[nest] unsupported SceShell (seg0=0x%08X seg1=0x%08X), no hooks",
		         (unsigned)mi.segments[0].memsz, (unsigned)mi.segments[1].memsz);
		return;
	}
	const shell_profile_t *p = shell_profile;
	nest_log("[nest] SceShell profile: %s", p->name);

	shell_seg1 = (uint32_t)mi.segments[1].vaddr;
	shell_seg0_lo = (uint32_t)seg0;
	shell_seg0_hi = (uint32_t)seg0 + mi.segments[0].memsz;

	if (code_matches(seg0 + p->wstr_assign, EXPECT_WSTR_ASSIGN, sizeof(EXPECT_WSTR_ASSIGN))) {
		const uint8_t *m = seg0 + p->wstr_assign + 10;
		paf_wempty = thumb_imm16(m) | (thumb_imm16(m + 6) << 16);
		shell_wstr_assign = (wstr_assign_t)((uint32_t)seg0 + p->wstr_assign + 1);
	} else {
		nest_log("[nest] wstring assign signature mismatch, copied items stay untitled");
	}
	if (code_matches(seg0 + p->folder_open, EXPECT_OPEN, sizeof(EXPECT_OPEN)) &&
	    code_matches(seg0 + p->folder_close, EXPECT_CLOSE, sizeof(EXPECT_CLOSE))) {
		uid_close = hook_at(&ref_close, ti.modid, p->folder_close, (const void *)close_hook);
		if (uid_close >= 0) {
			uid_open = hook_at(&ref_open, ti.modid, p->folder_open, (const void *)open_hook);
			if (uid_open < 0) {
				taiHookRelease(uid_close, ref_close);
				uid_close = -1;
			}
		}
	} else {
		nest_log("[nest] folder open/close signature mismatch");
	}
	if (code_matches(seg0 + p->fl_add, EXPECT_FL_ADD, sizeof(EXPECT_FL_ADD)) &&
	    code_matches(seg0 + p->fl_del, EXPECT_FL_DEL, sizeof(EXPECT_FL_DEL))) {
		uid_fladd = hook_at(&ref_fladd, ti.modid, p->fl_add, (const void *)fladd_hook);
		uid_fldel = hook_at(&ref_fldel, ti.modid, p->fl_del, (const void *)fldel_hook);
	} else {
		nest_log("[nest] folder list signature mismatch");
	}
	if (uid_fladd >= 0 && uid_fldel >= 0 &&
	    thumb_bl_targets(seg0 + p->gate1, seg0 + p->isfolder) &&
	    thumb_bl_targets(seg0 + p->gate2, seg0 + p->isfolder) &&
	    code_matches(seg0 + p->isfolder, EXPECT_ISFOLDER, sizeof(EXPECT_ISFOLDER))) {
		gate1_ret = ((uint32_t)seg0 + p->gate1 + 4) | 1u;
		gate2_ret = ((uint32_t)seg0 + p->gate2 + 4) | 1u;
		uid_isfolder = hook_at(&ref_isfolder, ti.modid, p->isfolder, (const void *)isfolder_hook);
	} else {
		nest_log("[nest] drop check signature mismatch");
	}
	if (code_matches(seg0 + p->iconcapture, EXPECT_ICONCAPTURE, sizeof(EXPECT_ICONCAPTURE))) {
		uid_capture = hook_at(&ref_capture, ti.modid, p->iconcapture, (const void *)capture_hook);
	} else {
		nest_log("[nest] icon capture signature mismatch");
	}
	if (uid_capture >= 0 &&
	    code_matches(seg0 + p->reload, EXPECT_RELOAD, sizeof(EXPECT_RELOAD)) &&
	    code_matches(seg0 + p->capfin, EXPECT_CAPFIN, sizeof(EXPECT_CAPFIN))) {
		uid_reload = hook_at(&ref_reload, ti.modid, p->reload, (const void *)reload_hook);
		if (uid_reload >= 0) {
			uid_capfin = hook_at(&ref_capfin, ti.modid, p->capfin, (const void *)capfin_hook);
			if (uid_capfin < 0) {
				taiHookRelease(uid_reload, ref_reload);
				uid_reload = -1;
			}
		}
	} else {
		nest_log("[nest] icon refresh signature mismatch");
	}
	{
		SceIoStat st;
		int sig = uid_capture >= 0 && shell_str_assign == 0 &&
		          code_matches(seg0 + p->str_assign, EXPECT_STR_ASSIGN, sizeof(EXPECT_STR_ASSIGN));
		if (sig)
			shell_str_assign = (str_assign_t)((uint32_t)seg0 + p->str_assign + 1);
		sig = uid_capture >= 0 && shell_str_assign &&
		      code_matches(seg0 + p->imgget, EXPECT_IMGGET, sizeof(EXPECT_IMGGET)) &&
		      bytes_equal(seg0 + p->cap_call, EXPECT_CAP_CALL, sizeof(EXPECT_CAP_CALL)) &&
		      bytes_equal(seg0 + p->cap_pos, EXPECT_CAP_POS, sizeof(EXPECT_CAP_POS)) &&
		      bytes_equal(seg0 + p->cap_list, EXPECT_CAP_LIST, sizeof(EXPECT_CAP_LIST)) &&
		      code_matches(seg0 + p->fldr_param, EXPECT_FLDR_PARAM, sizeof(EXPECT_FLDR_PARAM));
		if (!sig) {
			nest_log("[nest] sub-folder icon signature mismatch");
		} else if (sceIoGetstat(NEST_DATA_DIR "/nosubicon", &st) >= 0) {
			nest_log("[nest] nosubicon file found, sub-folder icons off");
		} else {
			const uint8_t *m = seg0 + p->fldr_param;
			fldr_loader = thumb_imm16(m) | (thumb_imm16(m + 8) << 16);
			paf_sempty = thumb_imm16(m + 4) | (thumb_imm16(m + 14) << 16);
			uid_imgget = hook_at(&ref_imgget, ti.modid, p->imgget, (const void *)imgget_hook);
			subicon_on = uid_imgget >= 0;
		}
	}
	if (code_matches(seg0 + p->mkfolderjob, EXPECT_MKFOLDERJOB, sizeof(EXPECT_MKFOLDERJOB)) &&
	    code_matches(seg0 + p->str_assign, EXPECT_STR_ASSIGN, sizeof(EXPECT_STR_ASSIGN))) {
		fldr_scan_appdb();
		shell_str_assign = (str_assign_t)((uint32_t)seg0 + p->str_assign + 1);
		uid_mkjob = hook_at(&ref_mkjob, ti.modid, p->mkfolderjob, (const void *)mkjob_hook);
	} else {
		nest_log("[tid] new folder signature mismatch");
	}
	nest_log("[nest] hooks: folder=%s list=%s drop=%s icon=%s refresh=%s subicon=%s titleId=%s",
	         uid_open >= 0 ? "ok" : "-", uid_fladd >= 0 && uid_fldel >= 0 ? "ok" : "-",
	         uid_isfolder >= 0 ? "ok" : "-", uid_capture >= 0 ? "ok" : "-",
	         uid_capfin >= 0 ? "ok" : "-", uid_imgget >= 0 ? "ok" : "-", uid_mkjob >= 0 ? "ok" : "-");
}

/* Shell ana ekrani kurdu mu? Global yonetici ve gorunum/kuyruk alanlari
 * dolana kadar bekle. Donus: beklenen sure (ms). */
static int wait_shell_ready(void)
{
	tai_module_info_t ti;
	ti.size = sizeof(ti);
	SceKernelModuleInfo mi;
	int waited = 0;
	/* SceShell modulu ve segment bilgisi */
	for (;;) {
		if (taiGetModuleInfo("SceShell", &ti) >= 0) {
			sceClibMemset(&mi, 0, sizeof(mi));
			mi.size = sizeof(mi);
			if (sceKernelGetModuleInfo(ti.modid, &mi) >= 0) break;
		}
		if (waited >= 60000) return waited;
		sceKernelDelayThread(100 * 1000); waited += 100;
	}
	shell_profile = shell_profile_find(mi.segments[0].vaddr,
		mi.segments[0].memsz, mi.segments[1].memsz);
	if (!shell_profile) return -1;
	uint32_t seg1 = (uint32_t)mi.segments[1].vaddr;
	for (;;) {
		uint32_t g = rd32(seg1 + shell_profile->seg1_g);
		if (g && rd32(g + 0x4) && rd32(g + 0x24)) break;
		if (waited >= 60000) {
			nest_log("[nest] no Shell ready signal, continuing anyway");
			return waited;
		}
		sceKernelDelayThread(100 * 1000); waited += 100;
	}
	sceKernelDelayThread(1000 * 1000);   /* ilk karelerin oturmasi icin */
	return waited + 1000;
}

static int setup(SceSize args, void *argp)
{
	(void)args; (void)argp;

	/* Her acilista yeni log: onceki oturumunki log.prev.txt olarak kalir */
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir(NEST_DATA_DIR, 0777);
	sceIoRemove(NEST_DATA_DIR "/log.prev.txt");
	sceIoRename(NEST_LOG_PATH, NEST_DATA_DIR "/log.prev.txt");
	nest_log("[nest] nestfolder 1.0.3");

	SceIoStat st;
	if (sceIoGetstat(NEST_DATA_DIR "/delay20", &st) >= 0) {
		sceKernelDelayThread(20 * 1000 * 1000);
		nest_log("[nest] delay20: waited 20 s");
	} else {
		int ms = wait_shell_ready();
		if (ms >= 0) nest_log("[nest] Shell ready, active after %d ms", ms);
	}

	if (sceIoGetstat(DISABLE_PATH, &st) >= 0) {
		nest_log("[nest] disable file found, plugin off");
		return sceKernelExitDeleteThread(0);
	}

	snapshot_mode = sceIoGetstat(NEST_DATA_DIR "/snapshot", &st) >= 0;
	if (snapshot_mode) nest_log("[nest] snapshot mode (old copy behaviour)");
	install_hooks();
	return sceKernelExitDeleteThread(0);
}

int module_start(SceSize args, void *argp)
{
	(void)args; (void)argp;
	SceUID th = sceKernelCreateThread("nestfolder_setup", setup,
		0x10000100 + 30, 0x4000, 0, 0, NULL);
	if (th >= 0) sceKernelStartThread(th, 0, NULL);
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	nest_clear();
	for (int k = 0; k < NPEND; k++) {
		if (pend_used[k]) { ref_drop(pend_rctrl[k]); pend_used[k] = 0; }
		ref_drop(pend_late[k]); pend_late[k] = 0;
	}
	if (uid_isfolder >= 0) taiHookRelease(uid_isfolder, ref_isfolder);
	if (uid_capture >= 0) taiHookRelease(uid_capture, ref_capture);
	if (uid_fladd >= 0) taiHookRelease(uid_fladd, ref_fladd);
	if (uid_fldel >= 0) taiHookRelease(uid_fldel, ref_fldel);
	if (uid_open  >= 0) taiHookRelease(uid_open,  ref_open);
	if (uid_close >= 0) taiHookRelease(uid_close, ref_close);
	if (uid_mkjob >= 0) taiHookRelease(uid_mkjob, ref_mkjob);
	if (uid_imgget >= 0) taiHookRelease(uid_imgget, ref_imgget);
	if (uid_capfin >= 0) taiHookRelease(uid_capfin, ref_capfin);
	if (uid_reload >= 0) taiHookRelease(uid_reload, ref_reload);
	return SCE_KERNEL_STOP_SUCCESS;
}
