// An installed fighter's own sounds and its name call (fighter_sound.h).

#include "fighter_sound.h"

#include "archive_unpack.h"
#include "host.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "nlohmann/json.hpp"

namespace host::fighter_sound {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  b[at] = (uint8_t)(v >> 24); b[at + 1] = (uint8_t)(v >> 16); b[at + 2] = (uint8_t)(v >> 8); b[at + 3] = (uint8_t)v;
}
void put16(std::vector<uint8_t>& b, size_t at, uint32_t v) { b[at] = (uint8_t)(v >> 8); b[at + 1] = (uint8_t)v; }
void push32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v);
}
uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[1] << 8 | p[0]); }

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

uint32_t fnv(uint32_t h, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
  return h;
}

bool read_file(const fs::path& path, std::vector<uint8_t>* out, uint64_t limit) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size == 0 || size > limit) return false;
  std::ifstream in(path, std::ios::binary);
  out->resize((size_t)size);
  return in && in.read((char*)out->data(), (std::streamsize)size);
}

bool write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  return out && out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

// What a fighter's bank may be: its sounds and samples (the largest retail fighter bank holds 62 sounds
// and 600 KB; the four largest fill the match's sound memory), its scripts.
constexpr uint32_t kMaxSounds = 1024, kMaxSampleBytes = 1u << 20, kMaxScripts = 4096, kMaxScriptWords = 1024;
constexpr uint64_t kMaxSsmFile = 4u << 20, kMaxSemFile = 1u << 20, kMaxSpk = 6u << 20, kMaxWav = 4u << 20;
// What the added banks' scripts may add to smash2.sem: the game keeps that file in its 512 KB audio heap
// (initialize.h HSD_DEFAULT_AUDIO_SIZE) beside each bank's sound table while it loads (the disc's: 129 KB).
constexpr uint64_t kMaxAddedScriptBytes = 64u << 10;

// The frames of the game's sound scripts a command waits (sysdolphin/baselib/axdriver.c parseWait).
uint32_t waits(uint32_t w) {
  switch (w >> 24) {
    case 0: return w & 0xFFFFFF;
    case 6: case 7: case 8: case 9: case 10: case 11: case 16: case 17: case 18: case 19: return (w >> 8) & 0xFFFF;
    case 12: case 13: return (w >> 16) & 0xFF;
    default: return 0;
  }
}

// One script from `at` of a file: its words up to and with the command that ends it (14 or 15).
bool read_script(const std::vector<uint8_t>& b, uint64_t at, std::vector<uint32_t>* out) {
  out->clear();
  if (at % 4) return false;
  while (at + 4 <= b.size() && out->size() < kMaxScriptWords) {
    const uint32_t w = be32(b.data() + at);
    out->push_back(w);
    at += 4;
    if ((w >> 24) == 14 || (w >> 24) == 15) return true;
  }
  return false;
}

// A bank's row from its group flags (group, load priority, unload priority, voice threshold): a
// fighter's bank (group 4) whose priorities the game's loader knows (0 to 4).
bool row_of(uint32_t flags, uint8_t row[4]) {
  row[0] = (uint8_t)(flags >> 24); row[1] = (uint8_t)(flags >> 16); row[2] = (uint8_t)(flags >> 8); row[3] = (uint8_t)flags;
  return row[0] == 4 && row[1] <= 4 && row[2] <= 4 && row[3] < 0x80;
}

// A scripts.sem of one bank, as MexManager writes it.
std::vector<uint8_t> write_group_sem(const std::vector<std::vector<uint32_t>>& scripts) {
  std::vector<uint8_t> b;
  push32(b, 0); push32(b, 0); push32(b, 1); push32(b, 0); push32(b, (uint32_t)scripts.size());
  const size_t table = b.size();
  b.resize(b.size() + 4 * scripts.size());
  push32(b, 0);
  for (size_t i = 0; i < scripts.size(); ++i) {
    put32(b, table + 4 * i, (uint32_t)b.size());
    for (uint32_t w : scripts[i]) push32(b, w);
  }
  return b;
}

// MexTool's sound.spk: "SPK" and a version, then little-endian {group flags, flags, .ssm size, script
// count}, the scripts {start, size[, name]}, the bank's file name, and the .ssm.
std::string read_spk(const std::vector<uint8_t>& spk, std::vector<uint8_t>* ssm, std::vector<std::vector<uint32_t>>* scripts,
                     uint32_t* group_flags) {
  if (spk.size() < 20 || std::memcmp(spk.data(), "SPK", 3) != 0) return "sound.spk is not a MexTool sound package";
  const int version = spk[3] == '2' ? 2 : 1;
  *group_flags = le32(spk.data() + 4);
  const uint32_t ssm_size = le32(spk.data() + 12), count = le32(spk.data() + 16);
  if (count == 0 || count > kMaxScripts) return "sound.spk's script count is not 1 to 4096";
  const uint64_t entry = version > 1 ? 12 : 8;
  uint64_t pos = 20;
  if (pos + entry * count + 1 > spk.size()) return "sound.spk's script list runs past its end";
  scripts->clear();
  for (uint32_t i = 0; i < count; ++i, pos += entry) {
    const uint64_t start = le32(spk.data() + pos), size = le32(spk.data() + pos + 4);
    if (size == 0 || size % 4 || size > 4 * kMaxScriptWords || start + size > spk.size())
      return "sound.spk's script " + std::to_string(i) + " is outside it";
    std::vector<uint32_t> words;
    for (uint64_t at = start; at < start + size; at += 4) words.push_back(be32(spk.data() + at));
    scripts->push_back(std::move(words));
  }
  pos += 1 + (uint64_t)spk[pos];
  if (ssm_size < 0x20 || pos + ssm_size > spk.size()) return "sound.spk's bank is outside it";
  ssm->assign(spk.begin() + (std::ptrdiff_t)pos, spk.begin() + (std::ptrdiff_t)(pos + ssm_size));
  return {};
}

// MexManager's sound.zip: group.json (its .ssm file and group flags), scripts.sem, the .ssm.
std::string read_sound_zip(const fs::path& zip, std::vector<uint8_t>* ssm, std::vector<std::vector<uint32_t>>* scripts,
                           uint32_t* group_flags) {
  host::archive::Zip z;
  std::string error;
  if (!z.open(zip, host::archive::Limits{32, 8ull << 20, 32ull << 20, 250}, &error)) return "sound.zip cannot be opened (" + error + ")";
  const auto entry = [&](const std::string& name) -> const host::archive::Entry* {
    for (const auto& e : z.entries())
      if (!e.folder() && lower(e.name) == lower(name)) return &e;
    return nullptr;
  };
  std::vector<uint8_t> group, sem;
  const host::archive::Entry* g = entry("group.json");
  const host::archive::Entry* s = entry("scripts.sem");
  if (!g || !s || !z.read(*g, &group, &error) || !z.read(*s, &sem, &error)) return "sound.zip has no group.json and scripts.sem";
  const json j = json::parse(group.begin(), group.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("fileName") || !j["fileName"].is_string())
    return "sound.zip's group.json names no bank";
  *group_flags = j.contains("groupFlags") && j["groupFlags"].is_number_integer() ? (uint32_t)j["groupFlags"].get<int64_t>() : 0;
  const host::archive::Entry* bank = entry(j["fileName"].get<std::string>());
  if (!bank || !z.read(*bank, ssm, &error)) return "sound.zip does not hold the bank its group.json names";
  return read_group_sem(sem, scripts);
}

uint32_t retail_call_of_bank(uint32_t bank) {
  // The announcer's call of the retail fighter whose sounds are bank `bank` (gm_80168C5C).
  switch (bank) {
    case 6: return 510000; case 7: return 510019; case 8: return 510001; case 9: return 510002;
    case 10: return 510004; case 11: return 510005; case 13: return 510011; case 14: return 510015;
    case 15: return 510016; case 16: return 510018; case 17: return 510020; case 18: return 510021;
    case 19: return 510022; case 20: return 510024; case 21: return 510026; case 22: return 510027;
    case 23: return 510028; case 24: return 510029; case 25: return 510013; case 26: return 510030;
    case 27: return 510033; case 28: return 510031; case 29: return 510010; case 30: return 510006;
    case 31: return 510012;
    default: return 0;
  }
}
constexpr uint32_t kRetailCalls = 510000, kRetailCallCount = 54;   // the disc's bank 51 scripts

// How many scripts bank `bank` of a smash2.sem has (0 when the file is not one).
uint32_t bank_scripts(const std::vector<uint8_t>& sem, uint32_t bank) {
  const auto word = [&](uint64_t at) -> uint64_t { return at + 4 <= sem.size() ? be32(sem.data() + at) : UINT64_MAX; };
  uint64_t o = 0;
  const uint64_t a = word(o);
  if (a > 4096) return 0;
  o += 4 + 4 * a;
  const uint64_t b = word(o);
  if (b > 4096) return 0;
  o += 4 + 4 * b;
  const uint64_t banks = word(o);
  if (banks <= bank || banks > 0x80) return 0;
  const uint64_t first = word(o + 4 + 4 * bank);
  const uint64_t next = bank + 1 < banks ? word(o + 4 + 4 * (bank + 1)) : word(o + 4 + 4 * banks);
  return next >= first && next != UINT64_MAX ? (uint32_t)(next - first) : 0;
}

// The sound table of an .ssm: where each sound's entry is.
struct SsmHead { uint32_t table, samples, count, base, data_at; };
SsmHead head_of(const std::vector<uint8_t>& ssm) {
  SsmHead h{be32(ssm.data()), be32(ssm.data() + 4), be32(ssm.data() + 8), be32(ssm.data() + 12), 0};
  h.data_at = (0x10 + h.table + 31) & ~31u;
  return h;
}

}  // namespace

// ---- checks ----

std::string check_ssm(const std::vector<uint8_t>& ssm, uint32_t* sounds, uint32_t* sample_bytes) {
  if (ssm.size() < 0x20 || ssm.size() > kMaxSsmFile) return "its bank is not 32 bytes to 4 MB";
  const SsmHead h = head_of(ssm);
  if (h.count == 0 || h.count > kMaxSounds) return "its bank's sound count is not 1 to 1024";
  if (h.table < 8 || 0x10ull + h.table > ssm.size()) return "its bank's sound table runs past the file";
  if (h.samples == 0 || h.samples > kMaxSampleBytes || (uint64_t)h.data_at + h.samples > ssm.size())
    return "its bank's samples run past the file (or pass 1 MB)";
  const uint64_t end = 0x10ull + h.table, nibbles = 2ull * h.samples;
  uint64_t at = 0x10;
  for (uint32_t i = 0; i < h.count; ++i) {
    if (at + 8 > end) return "its bank's sound table is shorter than its count";
    const uint32_t channels = be32(ssm.data() + at), rate = be32(ssm.data() + at + 4);
    if (channels < 1 || channels > 2) return "sound " + std::to_string(i) + " of its bank has " + std::to_string(channels) + " channels";
    if (rate == 0 || rate > 96000) return "sound " + std::to_string(i) + " of its bank has no usable rate";
    if (at + 8 + 0x40ull * channels > end) return "its bank's sound table is shorter than its count";
    for (uint32_t c = 0; c < channels; ++c) {
      const uint8_t* v = ssm.data() + at + 8 + 0x40 * c;
      const uint32_t loop_flag = be16(v), format = be16(v + 2), loop = be32(v + 4), last = be32(v + 8), current = be32(v + 12);
      if (loop_flag > 1 || format != 0) return "sound " + std::to_string(i) + " of its bank is not the console's ADPCM";
      if (last >= nibbles || current > last || loop > last)
        return "sound " + std::to_string(i) + " of its bank points outside its samples";
    }
    at += 8 + 0x40ull * channels;
  }
  *sounds = h.count;
  *sample_bytes = h.samples;
  return {};
}

std::string check_scripts(const std::vector<std::vector<uint32_t>>& scripts, uint32_t sounds) {
  if (scripts.empty() || scripts.size() > kMaxScripts) return "its script count is not 1 to 4096";
  for (size_t s = 0; s < scripts.size(); ++s) {
    const auto& w = scripts[s];
    const std::string which = "its script " + std::to_string(s);
    if (w.empty() || w.size() > kMaxScriptWords) return which + " is empty or too long";
    for (size_t i = 0; i < w.size(); ++i) {
      const uint32_t op = w[i] >> 24, arg = w[i] & 0xFFFFFF;
      if (op > 21 && op != 0xFD) return which + " has a command the game does not know";
      if (op == 1 && arg >= sounds) return which + " plays sound " + std::to_string(arg) + " of a bank of " + std::to_string(sounds);
      if (op == 3) {
        // The game goes back arg words and on (axdriver.c AXDriverInterp): the loop it repeats must be
        // inside the script and wait at least once, or the sound driver never returns.
        if (arg < 2 || arg > i + 1) return which + " loops outside itself";
        bool waited = false;
        for (size_t k = i + 1 - arg; k < i; ++k) waited = waited || waits(w[k]) > 0;
        if (!waited) return which + " loops without waiting";
      }
      if ((op == 14 || op == 15) && i + 1 != w.size()) return which + " goes on after its end";
    }
    const uint32_t last = w.back() >> 24;
    if (last != 14 && last != 15) return which + " has no end";
  }
  return {};
}

std::string read_group_sem(const std::vector<uint8_t>& sem, std::vector<std::vector<uint32_t>>* scripts) {
  if (sem.size() < 24 || sem.size() > kMaxSemFile) return "its scripts.sem is not 24 bytes to 1 MB";
  uint64_t o = 0;
  const auto word = [&](uint64_t at) -> uint64_t { return at + 4 <= sem.size() ? be32(sem.data() + at) : UINT64_MAX; };
  const uint64_t a = word(o);
  if (a > 1024) return "its scripts.sem is not one bank's";
  o += 4 + 4 * a;
  const uint64_t b = word(o);
  if (b != 0) return "its scripts.sem is not one bank's";
  o += 4;
  if (word(o) != 1 || word(o + 4) != 0) return "its scripts.sem is not one bank's";
  o += 8;
  const uint64_t count = word(o);
  if (count == 0 || count > kMaxScripts || o + 4 + 4 * count > sem.size()) return "its scripts.sem's script count is not 1 to 4096";
  o += 4;
  scripts->assign((size_t)count, {});
  for (uint64_t i = 0; i < count; ++i)
    if (!read_script(sem, word(o + 4 * i), &(*scripts)[(size_t)i])) return "its scripts.sem's script " + std::to_string(i) + " runs past the file";
  return {};
}

std::string load_bank(const fs::path& fighter_dir, Bank* out) {
  const fs::path dir = fighter_dir / kFolder;
  std::vector<uint8_t> sem, info;
  if (!read_file(dir / "bank.ssm", &out->ssm, kMaxSsmFile)) return "its sound\\bank.ssm cannot be read";
  if (!read_file(dir / "scripts.sem", &sem, kMaxSemFile)) return "its sound\\scripts.sem cannot be read";
  if (!read_file(dir / "sound.json", &info, 64 << 10)) return "its sound\\sound.json cannot be read";
  const json j = json::parse(info.begin(), info.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("groupFlags") || !j["groupFlags"].is_number_integer())
    return "its sound\\sound.json has no groupFlags";
  if (!row_of((uint32_t)j["groupFlags"].get<int64_t>(), out->row)) return "its bank's group is not a fighter's";
  std::string why = check_ssm(out->ssm, &out->sounds, &out->sample_bytes);
  if (why.empty()) why = read_group_sem(sem, &out->scripts);
  if (why.empty()) why = check_scripts(out->scripts, out->sounds);
  return why;
}

std::string read_wav(const std::vector<uint8_t>& wav, std::vector<int16_t>* pcm, uint32_t* rate) {
  if (wav.size() < 44 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0)
    return "it is not a WAV file";
  uint32_t channels = 0, bits = 0, format = 0;
  const uint8_t* data = nullptr;
  uint64_t data_size = 0;
  for (uint64_t at = 12; at + 8 <= wav.size();) {
    const uint64_t size = le32(wav.data() + at + 4);
    if (at + 8 + size > wav.size()) break;
    if (std::memcmp(wav.data() + at, "fmt ", 4) == 0 && size >= 16) {
      format = le16(wav.data() + at + 8);
      channels = le16(wav.data() + at + 10);
      *rate = le32(wav.data() + at + 12);
      bits = le16(wav.data() + at + 22);
    } else if (std::memcmp(wav.data() + at, "data", 4) == 0) {
      data = wav.data() + at + 8;
      data_size = size;
    }
    at += 8 + size + (size & 1);
  }
  if (format != 1 || bits != 16 || channels < 1 || channels > 2) return "it is not 16-bit PCM with one or two channels";
  if (*rate < 4000 || *rate > 48000) return "its rate is not 4000 to 48000";
  if (!data) return "it has no samples";
  const uint64_t frames = data_size / (2ull * channels);
  if (frames == 0 || frames > (uint64_t)*rate * 10) return "it is empty or longer than 10 seconds";
  pcm->resize((size_t)frames);
  for (uint64_t i = 0; i < frames; ++i) {
    int sum = 0;
    for (uint32_t c = 0; c < channels; ++c) sum += (int16_t)le16(data + (i * channels + c) * 2);
    (*pcm)[(size_t)i] = (int16_t)(sum / (int)channels);
  }
  return {};
}

// ---- DSP ADPCM ----

namespace {
int16_t clamp16(int64_t v) { return (int16_t)std::max<int64_t>(-32768, std::min<int64_t>(32767, v)); }
int16_t decode_one(int nibble, int scale, int c1, int c2, int h1, int h2) {
  int64_t v = ((int64_t)nibble * ((int64_t)1 << scale)) * 2048;
  v += 1024 + (int64_t)c1 * h1 + (int64_t)c2 * h2;
  return clamp16(v >> 11);
}
}  // namespace

Adpcm encode_adpcm(const std::vector<int16_t>& pcm) {
  Adpcm a;
  a.samples = (uint32_t)pcm.size();
  const size_t frames = (pcm.size() + 13) / 14;
  const auto x = [&](int64_t i) -> double { return i >= 0 && (size_t)i < pcm.size() ? pcm[(size_t)i] : 0.0; };
  // Each frame's best second-order predictor, weighted by its energy; six clusters of them, with
  // "none" and "the last sample" for silence and steps.
  struct V { double c1, c2, w; };
  std::vector<V> vs;
  for (size_t f = 0; f < frames; ++f) {
    double r11 = 0, r12 = 0, r22 = 0, r01 = 0, r02 = 0, e = 0;
    for (int64_t n = (int64_t)f * 14; n < (int64_t)f * 14 + 14; ++n) {
      const double x0 = x(n), x1 = x(n - 1), x2 = x(n - 2);
      r11 += x1 * x1; r12 += x1 * x2; r22 += x2 * x2; r01 += x0 * x1; r02 += x0 * x2; e += x0 * x0;
    }
    const double det = r11 * r22 - r12 * r12;
    if (e < 1.0 || r11 <= 0 || det <= 1e-9 * r11 * r22) continue;
    double c1 = (r01 * r22 - r02 * r12) / det, c2 = (r02 * r11 - r01 * r12) / det;
    c1 = std::max(-3.9, std::min(3.9, c1));
    c2 = std::max(-3.9, std::min(3.9, c2));
    vs.push_back({c1, c2, std::sqrt(e)});
  }
  double cs[8][2] = {{0, 0}, {1, 0}, {1.8, -0.82}, {1.5, -0.55}, {1.2, -0.3}, {1.9, -0.93}, {0.9, 0}, {1.6, -0.7}};
  if (vs.size() >= 6) {
    std::sort(vs.begin(), vs.end(), [](const V& p, const V& q) { return p.c1 < q.c1; });
    for (int k = 0; k < 6; ++k) { cs[2 + k][0] = vs[(vs.size() - 1) * k / 5].c1; cs[2 + k][1] = vs[(vs.size() - 1) * k / 5].c2; }
    for (int round = 0; round < 12; ++round) {
      double sum[6][3] = {};
      for (const V& v : vs) {
        int best = 0;
        double bd = 1e300;
        for (int k = 0; k < 6; ++k) {
          const double d = (v.c1 - cs[2 + k][0]) * (v.c1 - cs[2 + k][0]) + (v.c2 - cs[2 + k][1]) * (v.c2 - cs[2 + k][1]);
          if (d < bd) { bd = d; best = k; }
        }
        sum[best][0] += v.c1 * v.w; sum[best][1] += v.c2 * v.w; sum[best][2] += v.w;
      }
      for (int k = 0; k < 6; ++k)
        if (sum[k][2] > 0) { cs[2 + k][0] = sum[k][0] / sum[k][2]; cs[2 + k][1] = sum[k][1] / sum[k][2]; }
    }
  }
  for (int k = 0; k < 8; ++k)
    for (int j = 0; j < 2; ++j) a.coefs[2 * k + j] = clamp16((int64_t)std::lround(cs[k][j] * 2048.0));
  // Each frame: the predictor and scale whose decoding (from the decoded history) is nearest.
  a.data.assign(frames * 8, 0);
  int h1 = 0, h2 = 0;
  for (size_t f = 0; f < frames; ++f) {
    double best_err = 1e300;
    int best_p = 0, best_s = 0, best_h1 = 0, best_h2 = 0;
    int best_n[14] = {};
    for (int p = 0; p < 8; ++p) {
      const int c1 = a.coefs[2 * p], c2 = a.coefs[2 * p + 1];
      for (int s = 0; s <= 11; ++s) {
        int y1 = h1, y2 = h2, n[14];
        double err = 0;
        for (int i = 0; i < 14; ++i) {
          const double target = x((int64_t)f * 14 + i);
          const double pred = ((double)c1 * y1 + (double)c2 * y2 + 1024.0) / 2048.0;
          const long q = std::lround((target - pred) / (double)(1 << s));
          n[i] = (int)std::max(-8L, std::min(7L, q));
          const int16_t out = decode_one(n[i], s, c1, c2, y1, y2);
          err += (target - out) * (target - out);
          y2 = y1;
          y1 = out;
          if (err >= best_err) break;
        }
        if (err < best_err) {
          best_err = err; best_p = p; best_s = s; best_h1 = y1; best_h2 = y2;
          std::memcpy(best_n, n, sizeof n);
        }
      }
    }
    uint8_t* out = a.data.data() + f * 8;
    out[0] = (uint8_t)(best_p << 4 | best_s);
    for (int i = 0; i < 14; i += 2) out[1 + i / 2] = (uint8_t)((best_n[i] & 0xF) << 4 | (best_n[i + 1] & 0xF));
    h1 = best_h1;
    h2 = best_h2;
  }
  return a;
}

std::vector<int16_t> decode_adpcm(const Adpcm& a) {
  std::vector<int16_t> out;
  int h1 = 0, h2 = 0;
  for (size_t f = 0; f * 8 < a.data.size() && out.size() < a.samples; ++f) {
    const uint8_t* in = a.data.data() + f * 8;
    const int p = in[0] >> 4 & 7, s = in[0] & 0xF;
    for (int i = 0; i < 14 && out.size() < a.samples; ++i) {
      int n = (in[1 + i / 2] >> (i % 2 ? 0 : 4)) & 0xF;
      if (n >= 8) n -= 16;
      const int16_t y = decode_one(n, s, a.coefs[2 * p], a.coefs[2 * p + 1], h1, h2);
      out.push_back(y);
      h2 = h1;
      h1 = y;
    }
  }
  return out;
}

// ---- the package's sounds, at install ----

std::string take_package_sound(const fs::path& package, const fs::path& sound_dir, std::string* notes) {
  std::error_code ec;
  fs::path zip, spk, wav;
  for (fs::directory_iterator it(package, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string low = lower(it->path().filename().u8string());
    if (low == "sound.zip") zip = it->path();
    else if (low == "sound.spk") spk = it->path();
    else if (low == "namecall.wav") wav = it->path();
  }
  std::string taken;
  const auto note = [&](const std::string& what) { if (notes) *notes += (notes->empty() ? "" : "; ") + what; };
  fs::remove_all(sound_dir, ec);
  if (!zip.empty() || !spk.empty()) {
    std::vector<uint8_t> ssm, bytes;
    std::vector<std::vector<uint32_t>> scripts;
    uint32_t flags = 0, sounds = 0, samples = 0;
    std::string why;
    if (!spk.empty()) {
      why = read_file(spk, &bytes, kMaxSpk) ? read_spk(bytes, &ssm, &scripts, &flags) : "sound.spk cannot be read";
    } else {
      why = read_sound_zip(zip, &ssm, &scripts, &flags);
    }
    uint8_t row[4];
    if (why.empty()) why = check_ssm(ssm, &sounds, &samples);
    if (why.empty()) why = check_scripts(scripts, sounds);
    if (why.empty() && !row_of(flags, row)) why = "its bank's group is not a fighter's";
    if (!why.empty()) {
      note("its own sounds are not used (" + why + ")");
    } else {
      fs::create_directories(sound_dir, ec);
      const json info = {{"groupFlags", flags}, {"from", spk.empty() ? "sound.zip" : "sound.spk"}};
      const std::string text = info.dump(2);
      if (ec || !write_file(sound_dir / "bank.ssm", ssm) || !write_file(sound_dir / "scripts.sem", write_group_sem(scripts)) ||
          !write_file(sound_dir / "sound.json", std::vector<uint8_t>(text.begin(), text.end()))) {
        note("its own sounds could not be written");
        fs::remove_all(sound_dir, ec);
      } else {
        taken = std::to_string(sounds) + " sounds, " + std::to_string(scripts.size()) + " scripts";
      }
    }
  }
  if (!wav.empty()) {
    std::vector<uint8_t> bytes;
    std::vector<int16_t> pcm;
    uint32_t rate = 0;
    const std::string why = read_file(wav, &bytes, kMaxWav) ? read_wav(bytes, &pcm, &rate) : "it cannot be read";
    if (!why.empty()) {
      note("its namecall.wav is not used (" + why + ")");
    } else {
      fs::create_directories(sound_dir, ec);
      if (!ec && write_file(sound_dir / "namecall.wav", bytes)) taken += std::string(taken.empty() ? "" : ", ") + "its name call";
      else note("its namecall.wav could not be written");
    }
  }
  return taken;
}

// ---- the game's smash2.sem ----

std::vector<uint8_t> extend_sem(const std::vector<uint8_t>& sem, const std::map<int, std::vector<std::vector<uint32_t>>>& added,
                                const std::vector<std::vector<uint32_t>>& names, uint32_t names_from, uint32_t names_count,
                                uint32_t names_to, std::string* why) {
  const auto word = [&](uint64_t at) -> uint64_t { return at + 4 <= sem.size() ? be32(sem.data() + at) : UINT64_MAX; };
  uint64_t o = 0;
  const uint64_t a = word(o);
  if (a > 4096) { *why = "smash2.sem is not the game's"; return {}; }
  std::vector<uint32_t> a_words;
  for (uint64_t i = 0; i < a; ++i) a_words.push_back((uint32_t)word(4 + 4 * i));
  o += 4 + 4 * a;
  // The lists of pointers the file may have would move with the scripts: the game's has none.
  if (word(o) != 0) { *why = "smash2.sem has pointers this does not move"; return {}; }
  o += 4;
  const uint64_t banks = word(o);
  if (banks < 52 || banks > 0x80) { *why = "smash2.sem has no bank 51"; return {}; }
  o += 4;
  std::vector<uint32_t> starts;
  for (uint64_t i = 0; i < banks; ++i) starts.push_back((uint32_t)word(o + 4 * i));
  o += 4 * banks;
  const uint64_t scripts = word(o);
  if (scripts == 0 || scripts > 0x10000) { *why = "smash2.sem's script count is wrong"; return {}; }
  o += 4;
  const uint64_t offsets_at = o;
  o += 4 * scripts;
  if (word(o) != 0) { *why = "smash2.sem has pointers this does not move"; return {}; }
  for (uint64_t i = 0; i < banks; ++i)
    if (starts[i] > scripts || (i > 0 && starts[i] < starts[i - 1])) { *why = "smash2.sem's banks are out of order"; return {}; }
  std::vector<std::vector<std::vector<uint32_t>>> table((size_t)std::max<uint64_t>(banks, 56));
  for (uint64_t b = 0; b < banks; ++b) {
    const uint64_t first = starts[b], last = b + 1 < banks ? starts[b + 1] : scripts;
    for (uint64_t s = first; s < last; ++s) {
      std::vector<uint32_t> w;
      if (!read_script(sem, word(offsets_at + 4 * s), &w)) { *why = "smash2.sem's script " + std::to_string(s) + " runs past it"; return {}; }
      table[(size_t)b].push_back(std::move(w));
    }
  }
  if (names_to != 0) {
    for (auto& script : table[51])
      for (auto& w : script)
        if ((w >> 24) == 1 && (w & 0xFFFFFF) >= names_from && (w & 0xFFFFFF) < names_from + names_count)
          w = 1u << 24 | (names_to + ((w & 0xFFFFFF) - names_from));
    for (const auto& s : names) table[51].push_back(s);
  }
  for (const auto& [id, list] : added) {
    if (id <= 55 || id > kMaxBank) continue;
    if ((size_t)id >= table.size()) table.resize((size_t)id + 1);
    table[(size_t)id] = list;
  }
  std::vector<uint8_t> out;
  push32(out, (uint32_t)a);
  for (uint32_t w : a_words) push32(out, w);
  push32(out, 0);
  push32(out, (uint32_t)table.size());
  uint32_t total = 0;
  for (const auto& bank : table) { push32(out, total); total += (uint32_t)bank.size(); }
  push32(out, total);
  const size_t offsets = out.size();
  out.resize(out.size() + 4ull * total);
  push32(out, 0);
  uint32_t n = 0;
  for (const auto& bank : table)
    for (const auto& script : bank) {
      put32(out, offsets + 4ull * n++, (uint32_t)out.size());
      for (uint32_t w : script) push32(out, w);
    }
  return out;
}

// ---- the set the game is served ----

Set build(const std::vector<host::roster::Fighter>& fighters, const DiscSounds& disc, Session* session) {
  Set set;
  struct Own {
    int id = 0;
    uint32_t base = 0;
    std::string file;
    std::vector<uint8_t> ssm;
    std::vector<std::vector<uint32_t>> scripts;
    Bank bank;
  };
  std::vector<Own> own(fighters.size());
  std::vector<uint32_t> calls(fighters.size(), 0), fallbacks(fighters.size(), 0);
  std::vector<Adpcm> clips(fighters.size());
  std::vector<uint32_t> clip_rates(fighters.size(), 0);
  std::map<int, std::vector<std::vector<uint32_t>>> added;
  const bool can_add = !disc.sem_us.empty() || !disc.sem_jp.empty();
  uint64_t script_bytes = 0;
  uint32_t clip_bytes = 0;
  int clip_count = 0;
  for (size_t k = 0; k < fighters.size(); ++k) {
    const auto& f = fighters[k];
    uint32_t donor_bank = 55, call = 0;
    {
      std::vector<uint8_t> bytes;
      if (read_file(f.path / "fighter.json", &bytes, 8u << 20)) {
        const json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (j.is_object()) {
          if (j.contains("soundBank") && j["soundBank"].is_number_integer()) donor_bank = (uint32_t)j["soundBank"].get<int64_t>();
          if (j.contains("announcerCall") && j["announcerCall"].is_number_integer()) call = (uint32_t)j["announcerCall"].get<int64_t>();
        }
      }
    }
    // A call of the disc's own calls stays; another disc's number is its base fighter's call here.
    fallbacks[k] = call >= kRetailCalls && call < kRetailCalls + kRetailCallCount ? call : retail_call_of_bank(donor_bank);
    calls[k] = fallbacks[k];
    std::error_code ec;
    if (!fs::is_directory(f.path / kFolder, ec)) continue;
    // Its bank.
    if (fs::is_regular_file(f.path / kFolder / "bank.ssm", ec)) {
      Own& o = own[k];
      const std::string why = load_bank(f.path, &o.bank);
      if (!why.empty()) {
        set.log.push_back("sound: fighters\\" + f.folder + ": " + why + "; it keeps its base fighter's sounds (bank " +
                          std::to_string(donor_bank) + ")");
      } else if (!can_add) {
        set.log.push_back("sound: fighters\\" + f.folder + ": this disc's smash2.sem cannot take more banks; it keeps its base fighter's sounds");
      } else if ([&] {
                   uint64_t bytes = 0;
                   for (const auto& s : o.bank.scripts) bytes += 4 + 4 * (uint64_t)s.size();
                   if (script_bytes + bytes > kMaxAddedScriptBytes) return true;
                   script_bytes += bytes;
                   return false;
                 }()) {
        set.log.push_back("sound: fighters\\" + f.folder + ": the room for added sound scripts is full; it keeps its base fighter's sounds");
      } else {
        uint32_t h = fnv(2166136261u, o.bank.ssm.data(), o.bank.ssm.size());
        for (const auto& s : o.bank.scripts) h = fnv(h, s.data(), s.size() * 4);
        h = fnv(h, o.bank.row, 4);
        auto it = session->banks.find(h);
        if (it == session->banks.end() && session->next_bank <= kMaxBank &&
            session->next_sample + o.bank.sounds <= 0xFFFF) {
          it = session->banks.emplace(h, std::make_pair(session->next_bank++, session->next_sample)).first;
          session->next_sample += (o.bank.sounds + 15) & ~15u;
        }
        if (it == session->banks.end()) {
          set.log.push_back("sound: fighters\\" + f.folder + ": no bank number is left this run; it keeps its base fighter's sounds");
        } else {
          o.id = it->second.first;
          o.base = it->second.second;
          char name[32];
          std::snprintf(name, sizeof name, "nf_%08x.ssm", h);
          o.file = name;
          o.ssm = o.bank.ssm;
          put32(o.ssm, 12, o.base);
          o.scripts = o.bank.scripts;
          for (auto& s : o.scripts)
            for (auto& w : s)
              if ((w >> 24) == 1) w = 1u << 24 | (o.base + (w & 0xFFFFFF));
          added[o.id] = o.scripts;
        }
      }
    }
    // Its name call.
    if (fs::is_regular_file(f.path / kFolder / "namecall.wav", ec)) {
      std::vector<uint8_t> bytes;
      std::vector<int16_t> pcm;
      uint32_t rate = 0;
      std::string why = read_file(f.path / kFolder / "namecall.wav", &bytes, kMaxWav) ? read_wav(bytes, &pcm, &rate) : "it cannot be read";
      if (why.empty() && disc.names_us.empty()) why = "this disc's nr_name.ssm is not its own";
      if (why.empty()) {
        Adpcm a = encode_adpcm(pcm);
        const uint32_t bytes_needed = (uint32_t)((a.data.size() + 31) & ~(size_t)31);
        if (clip_bytes + bytes_needed > kMaxNameBytes || clip_count >= 64) why = "the calls' room is full";
        else { clip_bytes += bytes_needed; ++clip_count; clips[k] = std::move(a); clip_rates[k] = rate; }
      }
      if (!why.empty()) set.log.push_back("sound: fighters\\" + f.folder + ": its name call is not used (" + why + ")");
    }
  }
  // nr_name.ssm with the calls after the disc's sounds, its samples moved to kNamesBase on.
  std::vector<uint8_t> names_ssm;
  std::vector<std::vector<uint32_t>> name_scripts;
  uint32_t names_from = 0, names_count = 0;
  if (clip_count > 0) {
    uint32_t sounds = 0, samples = 0;
    std::string why = check_ssm(disc.names_us, &sounds, &samples);
    const SsmHead h = why.empty() ? head_of(disc.names_us) : SsmHead{};
    if (why.empty() && h.count + clip_count > 256) why = "too many sounds";
    if (why.empty() && (bank_scripts(disc.sem_us, 51) == 0 || bank_scripts(disc.sem_us, 51) + clip_count > 9999))
      why = "the disc's smash2.sem has no bank 51 to add them to";
    if (!why.empty()) {
      set.log.push_back("sound: the disc's nr_name.ssm cannot take the calls (" + why + "); the base fighters' calls are used");
      clip_count = 0;
    } else {
      names_from = h.base;
      names_count = h.count;
      std::vector<uint8_t> table(disc.names_us.begin() + 0x10, disc.names_us.begin() + 0x10 + h.table);
      std::vector<uint8_t> data(disc.names_us.begin() + h.data_at, disc.names_us.begin() + h.data_at + h.samples);
      // The calls are bank 51's scripts after the disc's own (54 on the retail disc).
      uint32_t index = h.count, script = bank_scripts(disc.sem_us, 51);
      for (size_t k = 0; k < fighters.size(); ++k) {
        if (clips[k].samples == 0) continue;
        data.resize((data.size() + 31) & ~(size_t)31);
        const uint32_t at = (uint32_t)data.size(), n = clips[k].samples;
        data.insert(data.end(), clips[k].data.begin(), clips[k].data.end());
        std::vector<uint8_t> e(8 + 0x40, 0);
        put32(e, 0, 1);
        put32(e, 4, clip_rates[k]);
        const uint32_t start = at * 2 + 2, last = at * 2 + (n - 1) / 14 * 16 + 2 + (n - 1) % 14;
        put32(e, 8 + 4, start);
        put32(e, 8 + 8, last);
        put32(e, 8 + 12, start);
        for (int c = 0; c < 16; ++c) put16(e, 8 + 0x10 + 2 * c, (uint16_t)clips[k].coefs[c]);
        put16(e, 8 + 0x32, clips[k].data[0]);
        table.insert(table.end(), e.begin(), e.end());
        // As the disc's calls: the sample, its send, priority and volume (bank 51's scripts).
        name_scripts.push_back({1u << 24 | (kNamesBase + index), 0x10000030u, 0x0400000Fu, 0x060000E5u, 0x0E000000u});
        calls[k] = kRetailCalls + script;
        ++index;
        ++script;
      }
      data.resize((data.size() + 31) & ~(size_t)31);
      names_ssm.assign(0x10, 0);
      put32(names_ssm, 0, (uint32_t)table.size());
      put32(names_ssm, 4, (uint32_t)data.size());
      put32(names_ssm, 8, index);
      put32(names_ssm, 12, kNamesBase);
      names_ssm.insert(names_ssm.end(), table.begin(), table.end());
      names_ssm.resize((names_ssm.size() + 31) & ~(size_t)31);
      names_ssm.insert(names_ssm.end(), data.begin(), data.end());
      uint32_t check_sounds = 0, check_samples = 0;
      const std::string bad = check_ssm(names_ssm, &check_sounds, &check_samples);
      if (!bad.empty()) {   // never expected: what was just made is checked like a package's
        set.log.push_back("sound: the calls could not be added (" + bad + ")");
        names_ssm.clear();
        name_scripts.clear();
        clip_count = 0;
        calls = fallbacks;
      }
    }
  }
  // Every fighter on its base fighter's bank, with its retail call: the disc's own files.
  const auto base_sounds_only = [&]() {
    set.files.clear();
    set.tails.clear();
    for (size_t k = 0; k < fighters.size(); ++k) {
      std::vector<uint8_t> t{1, 0, 0};
      push32(t, 0);
      t.insert(t.end(), {0, 0, 0, 0, 0, 0});
      push32(t, fallbacks[k]);
      push32(t, fallbacks[k]);
      set.tails.push_back(std::move(t));
    }
    set.serial = 0;
    return set;
  };
  if (added.empty() && name_scripts.empty()) return base_sounds_only();
  std::string why;
  std::vector<uint8_t> sem_us, sem_jp;
  if (!disc.sem_us.empty())
    sem_us = extend_sem(disc.sem_us, added, name_scripts, names_from, names_count, name_scripts.empty() ? 0 : kNamesBase, &why);
  if (why.empty() && !disc.sem_jp.empty() && !added.empty()) sem_jp = extend_sem(disc.sem_jp, added, {}, 0, 0, 0, &why);
  if (!why.empty()) {
    set.log.push_back("sound: " + why + "; every fighter keeps its base fighter's sounds");
    return base_sounds_only();
  }
  // English: the calls go with smash2.sem; Japanese keeps its own bank 51 (no calls added).
  if (!sem_us.empty()) set.files.push_back({"/audio/us/smash2.sem", std::move(sem_us)});
  if (!sem_jp.empty()) set.files.push_back({"/audio/smash2.sem", std::move(sem_jp)});
  if (!names_ssm.empty() && !name_scripts.empty()) set.files.push_back({"/audio/us/nr_name.ssm", std::move(names_ssm)});
  for (size_t k = 0; k < fighters.size(); ++k) {
    const Own& o = own[k];
    std::vector<uint8_t> t;
    t.push_back(1);
    t.push_back((uint8_t)o.id);
    t.push_back((uint8_t)o.file.size());
    t.insert(t.end(), o.file.begin(), o.file.end());
    push32(t, o.id ? o.bank.sample_bytes : 0);
    for (int i = 0; i < 4; ++i) t.push_back(o.id ? o.bank.row[i] : 0);
    t.push_back((uint8_t)((o.id ? o.scripts.size() : 0) >> 8));
    t.push_back((uint8_t)(o.id ? o.scripts.size() : 0));
    push32(t, calls[k]);
    push32(t, fallbacks[k]);
    set.tails.push_back(std::move(t));
    if (!o.id) continue;
    if (!disc.sem_us.empty()) set.files.push_back({"/audio/us/" + o.file, o.ssm});
    if (!disc.sem_jp.empty()) set.files.push_back({"/audio/" + o.file, o.ssm});
    set.log.push_back("sound: fighters\\" + fighters[k].folder + ": its own bank is bank " + std::to_string(o.id) + " (" + o.file + ", " +
                      std::to_string(o.bank.sounds) + " sounds from " + std::to_string(o.base) + ", " +
                      std::to_string(o.scripts.size()) + " scripts, " + std::to_string(o.bank.sample_bytes) + " sample bytes)");
  }
  for (size_t k = 0; k < fighters.size(); ++k)
    if (calls[k] != fallbacks[k])
      set.log.push_back("sound: fighters\\" + fighters[k].folder + ": its name call is " + std::to_string(calls[k]) + " (" +
                        std::to_string(clips[k].samples) + " samples at " + std::to_string(clip_rates[k]) + " Hz)");
  uint32_t serial = 2166136261u;
  for (const auto& f : set.files) {
    serial = fnv(serial, f.path.data(), f.path.size());
    serial = fnv(serial, f.data.data(), f.data.size());
  }
  set.serial = serial ? serial : 1;
  return set;
}

}  // namespace host::fighter_sound
