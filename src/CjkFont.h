#ifndef CJK_FONT_H
#define CJK_FONT_H

#include <Arduino.h>
#include <FS.h>

// 字体资源单例（DESIGN §5.5）：
//   classic 模式 = Unifont 16×16 平坦字库 /fonts/unifont.bin（tools/mk_unifont.py）
//   家族模式    = EPF1 包 /fonts/font-<name>-<px>.bin（tools/mk_fontpack.py 离线
//                 从任意 TTF 光栅化，含拉丁真实步进 + 全局基线度量）
//   家族由启动时扫 /fonts 自动发现，加新字体 = 转一个包拷进去。
//   （/fonts 缺失时回根目录找，兼容旧布局）
// 缺字回退链：家族包 → Unifont → 16×16 空心方框。
//
// Unifont 数据：65536 × 32 字节平坦布局 = offset cp*32，免索引直接 seek。
// EPF1 包（little-endian，详见 tools/mk_fontpack.py 头注释）：
//   32B 头(ver/sizePx/baselineY/nGlyph/maxDescent/maxAdv/indexOff/entriesOff/name)
//   + u32[257] 桶索引(index[b]=首个 cp>>8>=b 记录相对 entries 的字节偏移)
//   + 变长记录(u16 cp, u8 adv, i8 xoff, i8 yoff, u8 w, u8 h, bits)
//
// 加载策略（两层都试）：整包 ps_malloc 进 PSRAM → 普通 malloc → SD 模式
//   （只缓存当前桶 ≤32KB，PSRAM 分配）。
class CjkFont {
public:
  static const int GLYPH_PX    = 16;
  static const int GLYPH_BYTES = 32;
  static const int MAXFAM      = 8;   // classic 之外最多 7 个 SD 家族

  // 挂载后调用（内部先 Storage::begin() + Unifont + 扫家族）。
  // Unifont 缺失不致命：画方框；英文渲染不受影响。
  bool begin(const char* path = "/fonts/unifont.bin");
  bool ok() const { return m_ok; }

  // —— 家族管理 ——
  int  familyCount() const { return m_nfam; }          // 0 恒为 classic
  const char* familyName(int i) const;                 // 越界 → "classic"
  int  familyIndex(const char* name) const;            // 找不到 → 0(classic)
  int  currentFamily() const { return m_fam; }
  // 装载家族在 sizePx 档的包（sizePx∈{16,24,32}）。idx=0 → 卸载回 classic。
  // 失败（文件缺/坏）→ 回退 classic 并返回 false。
  bool setFamily(int idx, uint8_t sizePx);
  bool packActive() const { return m_fam > 0; }

  // —— 家族模式度量（packActive() 时有效；否则返回 0）——
  int packAdv(uint32_t cp);          // 包内步进；-1 = 包里没有
  int baselineY() const  { return m_baseY; }
  int maxDescent() const { return m_desc; }
  int maxAdv() const     { return m_maxAdv; }

  // —— Unifont 查询（classic 模式 / 回退）——
  const uint8_t* glyph(uint32_t cp);

  // 画字形到当前画布（pen x, 行顶 y）。家族模式画紧凑 ink（对齐基线），
  // 否则 Unifont 字形按 size 最近邻放大。只写黑像素，背景需已填白。
  // 注意：这里不负责步进 —— 调用方用 packAdv()/档位步进推进 x。
  void drawGlyph(int x, int y, uint32_t cp, int size = 16);

private:
  struct Rec {                 // 包记录解析结果
    int adv, xoff, yoff, w, h;
    const uint8_t* bits;       // 指向包内存或桶缓存（绘制期间保持有效）
  };
  bool findPack(uint32_t cp, Rec* r);
  bool loadBucket(int b);                      // SD 模式：换桶装载
  void unloadPack();

  // Unifont
  File m_file;
  uint8_t* m_ram = nullptr;
  uint8_t m_buf[GLYPH_BYTES];
  uint32_t m_cached = 0xFFFFFFFF;
  bool m_ok = false;

  // 家族
  char m_famNames[MAXFAM][16] = {"classic"};
  int  m_nfam = 1;
  int  m_fam = 0;              // 当前家族（0=classic）
  File m_packFile;             // SD 模式保持打开
  uint8_t* m_pack = nullptr;   // 整包（PSRAM/heap）或 nullptr
  size_t m_packSize = 0;
  uint32_t m_index[257];       // 桶索引常驻 RAM（1KB）
  uint32_t m_entriesOff = 0;
  uint8_t m_baseY = 0, m_desc = 0, m_maxAdv = 0, m_packSizePx = 0;
  // SD 模式桶缓存
  uint8_t* m_buk = nullptr;
  size_t m_bukLen = 0;
  int m_bukId = -1;
};

extern CjkFont CJK;   // 全局单例：字库是共享只读资源

#endif  // CJK_FONT_H
