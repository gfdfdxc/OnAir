// Recording paths must preserve UTF-8 independently of the Windows ANSI code page.
#include "dect2/common/u8file.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/source/iq_record.h"
#include "dect2/dab/dab.h"
#include "dect2/tv/es_save.h"
#include "dect2/tv/recorder.h"
#include "dect2/tv/ts_record.h"
#include "dect2/tv/ts_remux.h"
#include "dect2/rs/dvbsi.h"
#include "dect2/tv/ts_proto.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace dect2;
namespace fs = std::filesystem;
static int failures = 0;
static void check(bool ok, const char* what) { if (!ok) { std::cout << "FAIL: " << what << '\n'; failures++; } }
static std::string utf8(const fs::path& path) { const auto v = path.u8string(); return {v.begin(), v.end()}; }
static std::string read(const fs::path& path) { std::ifstream f(path, std::ios::binary); return {(std::istreambuf_iterator<char>(f)), {}}; }
using Bytes = std::vector<uint8_t>;
static Bytes sectionPacket(int pid, Bytes s) {
    const int length = (int)s.size() + 1; s[1] = (uint8_t)((s[1] & 0xF0) | (length >> 8)); s[2] = (uint8_t)length;
    const uint32_t crc = tsp::crc32(s.data(), (int)s.size());
    for (int shift = 24; shift >= 0; shift -= 8) s.push_back((uint8_t)(crc >> shift));
    Bytes packet(188, 0xFF); packet[0] = 0x47; packet[1] = (uint8_t)(0x40 | (pid >> 8)); packet[2] = (uint8_t)pid; packet[3] = 0x10; packet[4] = 0;
    std::copy(s.begin(), s.end(), packet.begin() + 5); return packet;
}
static Bytes servicePacket(const std::string& name, const std::string& provider) {
    Bytes d = {0x48, (uint8_t)(3 + name.size() + provider.size()), 1, (uint8_t)provider.size()};
    d.insert(d.end(), provider.begin(), provider.end()); d.push_back((uint8_t)name.size()); d.insert(d.end(), name.begin(), name.end());
    Bytes s = {0x42, 0xF0, 0, 0, 1, 0xC1, 0, 0, 0xFF, 1, 0xFF, 0, 1, 0xFC, 0x80, (uint8_t)d.size()};
    s.insert(s.end(), d.begin(), d.end()); return sectionPacket(0x11, s);
}
static void metadata(const fs::path& root) {
    const auto pat = sectionPacket(0, {0, 0xB0, 0, 0, 1, 0xC1, 0, 0, 0, 1, 0xE1, 0});
    const auto pmt = sectionPacket(0x100, {2, 0xB0, 0, 0, 1, 0xC1, 0, 0, 0xE1, 1, 0xF0, 0, 3, 0xE1, 1, 0xF0, 0});
    const auto original = servicePacket(std::string("\xD6\xD0\xCE\xC4", 4), "provider");
    const auto path = root / u8file("广播原始元数据.ts");
    { TsWriter writer; check(writer.open(utf8(path)), "TS writer opens Unicode path"); writer.write(original.data(), original.size()); writer.close(); }
    check(read(path) == std::string(original.begin(), original.end()), "TS recording preserves the exact original metadata bytes");
    for (int kind = 0; kind < 5; kind++) {
        const bool inherited = kind == 1 || kind == 3 || kind == 4;
        std::string inputName = std::string(1, '\x15') + std::string(58, 'a') + "日本語";
        std::string inputProvider = std::string(1, '\x15') + std::string(58, 'b') + "한국어";
        std::string overrideName = std::string(59, 'a') + "中文😀", overrideProvider = std::string(59, 'b') + "한국어";
        std::string expectedName(inherited ? 58 : 59, 'a'), expectedProvider(inherited ? 58 : 59, 'b');
        if (kind == 2) { overrideName = "中文 日本語 한국어 😀"; overrideProvider = "Café 提供者"; expectedName = overrideName; expectedProvider = overrideProvider; }
        if (kind == 3) { inputName = std::string(1, '\x15') + "中文 日本語 한국어 😀"; inputProvider = std::string(1, '\x15') + "Café 提供者"; expectedName = inputName.substr(1); expectedProvider = inputProvider.substr(1); }
        if (kind == 4) { inputName = std::string("\x10\x00\x01", 3) + "Caf\xE9"; inputProvider = "Provider"; expectedName = inputName; expectedProvider = inputProvider; }
        size_t next = 0;
        const auto sdt = servicePacket(inputName, inputProvider);
        const std::vector<Bytes> packets = {pat, pmt, sdt};
        TsRemuxInput in; in.rate = [] { return 10000.; }; in.pop = [&](uint8_t* p) { const auto& s = packets[next++ % packets.size()]; std::copy(s.begin(), s.end(), p); return true; };
        if (!inherited) { in.name = overrideName; in.provider = overrideProvider; }
        TsRemuxConfig cfg; cfg.outBitrate = 20000; TsRemux remux({in}, cfg); bool seen = false;
        for (int i = 0; i < 1000 && !seen; i++) {
            uint8_t p[188]; remux.next(p); if (((p[1] & 31) << 8 | p[2]) != 0x11 || !(p[1] & 0x40)) continue;
            const auto* s = p + 5; const int len = 3 + ((s[1] & 15) << 8 | s[2]);
            int tsid = 0, onid = 0; std::vector<tsp::SdtService> services;
            check(tsp::crc32(s, len) == 0, "remux SDT CRC");
            if (!tsp::parseSdt(s, len, tsid, onid, services) || services.empty()) continue;
            const auto& service = services[0];
            std::string name((const char*)s + service.name.off, service.name.len), provider((const char*)s + service.provider.off, service.provider.len);
            if (kind == 2 || kind == 3) check(!name.empty() && name[0] == '\x15', "non-ASCII TS service name declares UTF-8");
            if (!name.empty() && name[0] == '\x15') name.erase(0, 1);
            if (!provider.empty() && provider[0] == '\x15') provider.erase(0, 1);
            check(name == expectedName, "remux service name ends on a complete UTF-8 character");
            check(provider == expectedProvider, "remux provider ends on a complete UTF-8 character"); seen = true;
        }
        check(seen, "remux emits service metadata");
    }
}
static void audio(const std::string& path, AudioRecFormat format) {
    auto& rec = AudioRecorder::instance(); AudioRecOptions opts; opts.format = format; opts.tag = "中文 日本語 한국어";
    std::string error; static int source;
    check(rec.start(path, opts, error), "start PCM recording");
    std::vector<float> samples(960 * 2, 0.125f);
    for (int i = 0; i < 12; ++i) { AudioRecorder::offer(&source, samples.data(), 960, 48000); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    rec.stop();
    check(rec.stats().error.empty(), "PCM recording has no write error");
    check(fs::exists(u8file(path)) && !read(u8file(path)).empty(), "PCM recording lands at the requested UTF-8 path");
}
int main() {
    const auto root = fs::temp_directory_path() / "onair-recording-unicode";
    std::error_code ec;
    fs::create_directories(root, ec);
    const std::string stem = "中文_日本語_한국어_é_😀";
    // A non-ASCII leaf in an ASCII directory catches narrow fopen without depending on folder creation.
    const auto elementary = root / u8file(stem + ".mp2");
    { EsSink sink; check(sink.open(utf8(elementary)), "open elementary stream"); const uint8_t data[] = {0xFF, 0xFD, 0x80, 0xA0}; sink.write(data, sizeof data); sink.close(); }
    check(read(elementary) == std::string("\xFF\xFD\x80\xA0", 4), "elementary stream uses the exact requested filename and bytes");
    audio(utf8(root / u8file(stem + ".wav")), AudioRecFormat::Wav);
    audio(utf8(root / u8file(stem + ".flac")), AudioRecFormat::Flac);
    audio(utf8(root / u8file("录制目录_日本語") / u8file(stem + ".wav")), AudioRecFormat::Wav);
    { DabAudio receiver; receiver.setSilent(true); receiver.select(1, false, 128); std::string error;
      check(receiver.startRecording(utf8(root / u8file("数字广播_" + stem)), DabRecFormat::Wav, true, &error), "DAB WAV recording starts"); receiver.stopRecording();
      check(read(root / u8file("数字广播_" + stem + ".wav")).substr(0, 4) == "RIFF", "DAB WAV filename");
      check(fs::exists(root / u8file("数字广播_" + stem + ".txt")), "DAB label log filename"); }
    { IqRecorder rec; rec.setSigmf(true, 474e6, "测试 日本語 한국어"); std::string error;
      const std::string base = utf8(root / u8file("射频_" + stem));
      check(rec.start(base + ".cs8", FileFormat::CS8, 1e6, error), "IQ recording starts");
      const cf32 samples[16] = {}; rec.push(samples, 16); rec.stop();
      const std::string metadata = read(u8file(base + ".sigmf-meta"));
      check(!metadata.empty() && metadata.find("测试 日本語 한국어") != std::string::npos, "SigMF metadata filename and UTF-8 description");
      check(fs::exists(u8file(base + ".sigmf-data")) && fs::equivalent(u8file(base + ".cs8"), u8file(base + ".sigmf-data"), ec), "SigMF data alias filename and file identity"); }
    { IqRecorder rec; rec.setSigmf(true); std::string error; const auto path = root / u8file("直接录制_" + stem + ".sigmf-data");
      check(rec.start(utf8(path), FileFormat::CS8, 1e6, error), "record directly to the SigMF data name");
      const cf32 samples[16] = {}; rec.push(samples, 16); rec.stop();
      check(fs::exists(path) && read(path).size() == 32, "SigMF alias handling retains the recorded samples"); }
    const auto library = root / "library"; fs::create_directories(library, ec);
    { RecordingEntry e; e.tsPath = utf8(library / u8file(stem + ".ts")); e.title = "节目_日本語_한국어"; e.serviceName = "服务提供者";
      std::ofstream(u8file(e.tsPath), std::ios::binary).write("TS", 2); check(writeSidecar(e), "write Unicode recording sidecar");
      const auto entries = listRecordings(utf8(library));
      check(entries.size() == 1 && fs::equivalent(u8file(entries[0].tsPath), u8file(e.tsPath), ec) && entries[0].title == e.title && entries[0].serviceName == e.serviceName && entries[0].hasSidecar, "recording library matches Unicode file with its metadata"); }
    metadata(root);
    fs::remove_all(root, ec);
    std::cout << "recording_unicode: " << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
