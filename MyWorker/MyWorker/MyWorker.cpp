#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <locale>
#include <stdexcept>


namespace fs = std::filesystem;

// ---------------------------------------------------------------- параметри
struct Options {
    int K = 0;                   // кількість робочих потоків (обов'язковий)
    fs::path dir, out;
    int dark = -1, bright = -1;
    int repeat = 1;              // скільки разів повторити корисне оброблення
    bool generate = false;
    int count = 24;              // скільки зображень генерувати
};

static bool ParseArgs(int argc, char** argv, Options& o) {
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("немає значення для " + a);
                return argv[++i];
                };
            if (a == "--threads")       o.K = std::stoi(next());
            else if (a == "--dir")      o.dir = next();
            else if (a == "--out")      o.out = next();
            else if (a == "--dark")     o.dark = std::stoi(next());
            else if (a == "--bright")   o.bright = std::stoi(next());
            else if (a == "--repeat")   o.repeat = std::stoi(next());
            else if (a == "--count")    o.count = std::stoi(next());
            else if (a == "--generate") o.generate = true;
            else throw std::runtime_error("невідомий параметр " + a);
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Помилка параметрів: %s\n", e.what());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- генератор
static int Generate(const Options& o) {
    if (o.dir.empty() || o.count < 1) {
        std::fprintf(stderr, "Використання: --generate --dir <каталог> --count N (16..32)\n");
        return 2;
    }
    std::error_code ec;
    fs::create_directories(o.dir, ec);
    const int W = 1024, H = 1024;
    for (int i = 0; i < o.count; ++i) {
        int kind = i % 3;          // 0 - стале, 1 - градієнт, 2 - шахівниця
        int var = i / 3;
        char name[64];
        const char* prefix = kind == 0 ? "const" : (kind == 1 ? "grad" : "chess");
        std::snprintf(name, sizeof(name), "%s_%02d.pgm", prefix, i);
        std::ofstream f(o.dir / name, std::ios::binary);   // binary: лише LF, без BOM
        if (!f) { std::fprintf(stderr, "Не вдалося створити %s\n", name); return 3; }
        f << "P2\n" << W << ' ' << H << "\n255\n";
        int c = (var * 37 + 10) % 256;
        int bs = 8 << (var % 3);                            // розмір клітинки 8/16/32
        std::string line;
        for (int y = 0; y < H; ++y) {
            line.clear();
            for (int x = 0; x < W; ++x) {
                int v;
                if (kind == 0) v = c;
                else if (kind == 1) v = std::min(255, (x + y) * 255 / 2046 + var);
                else v = (((x / bs) + (y / bs)) & 1) ? 255 : 0;
                line += std::to_string(v);
                line += (x + 1 < W) ? ' ' : '\n';
            }
            f << line;
        }
    }
    std::printf("Створено %d файлів у %s\n", o.count, o.dir.string().c_str());
    return 0;
}

// ---------------------------------------------------------------- результати
struct FileResult {
    std::string name;
    bool ok = false;
    std::string error;
    long long width = 0, height = 0;
    unsigned long long hist[256];          // приватна гістограма цього файла
    unsigned long long count = 0, sum = 0; // 64-бітові
    int minV = 0, maxV = 0;
    double mean = 0, median = 0, darkFrac = 0, brightFrac = 0;
    FileResult() { std::memset(hist, 0, sizeof(hist)); }   // hist[256] обнулено для кожного нового файла
};

struct ThreadContext {                     // стабільна структура: вектор не змінюється після створення
    int index = 0;
    size_t begin = 0, end = 0;             // діапазон індексів файлів [begin, end)
    const std::vector<fs::path>* files = nullptr;
    const Options* opt = nullptr;
    std::vector<FileResult> results;       // власний вихід потоку
    DWORD exitCode = 0;
};

// ---------------------------------------------------------------- розбір PGM
static bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

struct Tokenizer {
    const char* p; const char* end;
    bool Next(const char*& b, const char*& e) {
        while (p < end && IsWs(*p)) ++p;
        if (p >= end) return false;
        b = p;
        while (p < end && !IsWs(*p)) ++p;
        e = p;
        return true;
    }
};

// тільки цифри; мінус, плюс, крапка, літери -> помилка
static bool ToInt(const char* b, const char* e, long long& v) {
    if (b == e) return false;
    v = 0;
    for (const char* c = b; c < e; ++c) {
        if (*c < '0' || *c > '9') return false;
        v = v * 10 + (*c - '0');
        if (v > 1000000000LL) return false;
    }
    return true;
}

static bool ReadAll(const fs::path& p, std::string& buf) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    if (n < 0) return false;
    buf.resize((size_t)n);
    if (n > 0) f.read(&buf[0], n);
    return !f.fail();
}

static FileResult ProcessFile(const fs::path& path, int dark, int bright) {
    FileResult r;
    r.name = path.filename().string();
    std::string buf;
    if (!ReadAll(path, buf)) { r.error = "не вдалося прочитати файл"; return r; }

    Tokenizer t{ buf.data(), buf.data() + buf.size() };
    const char* b, * e;
    long long w = 0, h = 0, mx = 0;

    if (!t.Next(b, e) || (e - b) != 2 || b[0] != 'P' || b[1] != '2') { r.error = "заголовок не P2"; return r; }
    if (!t.Next(b, e) || !ToInt(b, e, w) || w <= 0) { r.error = "хибна ширина"; return r; }
    if (!t.Next(b, e) || !ToInt(b, e, h) || h <= 0) { r.error = "хибна висота"; return r; }
    if (!t.Next(b, e) || !ToInt(b, e, mx) || mx != 255) { r.error = "maxval має бути 255"; return r; }
    r.width = w; r.height = h;

    unsigned long long N = (unsigned long long)w * (unsigned long long)h;
    if (N > 2000000000ULL) { r.error = "завеликі розміри"; return r; }

    unsigned long long cnt = 0, sum = 0;
    long long v = 0;
    while (t.Next(b, e)) {
        if (cnt >= N) { r.error = "зайві значення після width*height пікселів"; return r; }
        if (!ToInt(b, e, v) || v > 255) { r.error = "нечисловий токен або піксель поза 0..255"; return r; }
        r.hist[v]++;
        sum += (unsigned long long)v;
        ++cnt;
    }
    if (cnt != N) { r.error = "відсутні значення пікселів"; return r; }

    r.count = cnt; r.sum = sum;

    // k-й елемент (нумерація з 0) - накопиченням частот
    auto kth = [&](unsigned long long k) -> int {
        unsigned long long acc = 0;
        for (int i = 0; i < 256; ++i) { acc += r.hist[i]; if (acc > k) return i; }
        return 255;
        };
    for (int i = 0; i < 256; ++i)   if (r.hist[i]) { r.minV = i; break; }
    for (int i = 255; i >= 0; --i)  if (r.hist[i]) { r.maxV = i; break; }
    r.mean = (double)sum / (double)N;
    r.median = (N % 2 == 1) ? (double)kth(N / 2)
        : ((double)kth(N / 2 - 1) + (double)kth(N / 2)) / 2.0;
    unsigned long long d = 0, br = 0;
    for (int i = 0; i < 256; ++i) {
        if (i < dark)   d += r.hist[i];     // строго менше порога
        if (i > bright) br += r.hist[i];    // строго більше порога
    }
    r.darkFrac = (double)d / (double)N;
    r.brightFrac = (double)br / (double)N;
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- виведення
static std::string Fmt(double v) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o.precision(10);
    o << v;
    return o.str();
}

static bool WriteHistogram(const fs::path& outDir, const FileResult& r) {
    std::string stem = fs::path(r.name).stem().string();
    std::ofstream f(outDir / ("histogram_" + stem + ".csv"), std::ios::binary);
    if (!f) return false;
    f << "value;count\n";
    for (int i = 0; i < 256; ++i) f << i << ';' << r.hist[i] << '\n';   // усі 256 рядків
    return !f.fail();
}

// ---------------------------------------------------------------- потік
static DWORD WINAPI WorkerProc(LPVOID p) {
    ThreadContext* c = static_cast<ThreadContext*>(p);
    DWORD errors = 0;
    for (size_t i = c->begin; i < c->end; ++i) {
        FileResult r;
        for (int rep = 0; rep < c->opt->repeat; ++rep) {    // повтор корисного оброблення тих самих даних
            r = ProcessFile((*c->files)[i], c->opt->dark, c->opt->bright);
            if (!r.ok) break;
        }
        if (r.ok && !WriteHistogram(c->opt->out, r)) { r.ok = false; r.error = "не вдалося записати histogram"; }
        if (!r.ok) ++errors;
        c->results.push_back(std::move(r));
    }
    c->exitCode = errors;
    return errors;   // код завершення потоку = кількість помилкових файлів
}

static bool IsPgm(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& ch : e) ch = (char)std::tolower((unsigned char)ch);
    return e == ".pgm";
}

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options o;
    if (!ParseArgs(argc, argv, o)) return 2;
    if (o.generate) return Generate(o);

    if (o.K < 1 || o.K > 32) { std::fprintf(stderr, "--threads K: потрібно 1..32\n"); return 2; }
    if (o.dir.empty() || o.out.empty()) { std::fprintf(stderr, "Потрібні --dir і --out\n"); return 2; }
    if (!(0 <= o.dark && o.dark < o.bright && o.bright <= 255)) {
        std::fprintf(stderr, "Потрібно 0 <= dark < bright <= 255\n"); return 2;
    }
    if (o.repeat < 1) { std::fprintf(stderr, "--repeat має бути >= 1\n"); return 2; }

    // список .pgm, відсортований за іменем
    std::vector<fs::path> files;
    try {
        for (const auto& en : fs::directory_iterator(o.dir))
            if (en.is_regular_file() && IsPgm(en.path())) files.push_back(en.path());
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Не вдалося прочитати каталог: %s\n", e.what());
        return 3;
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
        });
    if (files.empty()) { std::fprintf(stderr, "У каталозі немає .pgm файлів\n"); return 3; }
    if (files.size() < (size_t)o.K)
        std::fprintf(stderr, "Попередження: файлів (%zu) менше, ніж потоків (%d)\n", files.size(), o.K);

    std::error_code ec;
    fs::create_directories(o.out, ec);

    // K стабільних контекстів (вектор більше не змінюється)
    std::vector<ThreadContext> ctx((size_t)o.K);
    size_t n = files.size(), pos = 0;
    for (int i = 0; i < o.K; ++i) {
        size_t cnt = n / (size_t)o.K + ((size_t)i < n % (size_t)o.K ? 1 : 0);
        ctx[i].index = i;
        ctx[i].begin = pos;
        ctx[i].end = pos + cnt;
        ctx[i].files = &files;
        ctx[i].opt = &o;
        pos += cnt;
    }

    // створення потоків призупиненими
    std::vector<HANDLE> h((size_t)o.K, nullptr);
    std::vector<DWORD> tid((size_t)o.K, 0);
    for (int i = 0; i < o.K; ++i) {
        h[i] = CreateThread(nullptr, 0, WorkerProc, &ctx[i], CREATE_SUSPENDED, &tid[i]);
        if (!h[i]) {
            std::fprintf(stderr, "CreateThread #%d: помилка %lu\n", i, (unsigned long)GetLastError());
            return 4;
        }
    }

    // manifest.csv: TID -> файли; закрити до запуску
    {
        std::ofstream m(o.out / "manifest.csv", std::ios::binary);
        m << "thread_index;tid;first_file;file_count;files\n";
        for (int i = 0; i < o.K; ++i) {
            m << i << ';' << (unsigned long)tid[i] << ';' << ctx[i].begin << ';'
                << (ctx[i].end - ctx[i].begin) << ';';
            for (size_t j = ctx[i].begin; j < ctx[i].end; ++j) {
                if (j > ctx[i].begin) m << '|';
                m << files[j].filename().string();
            }
            m << '\n';
        }
    }

    std::printf("MyWorker: PID=%lu, потоків K=%d, файлів=%zu, dark=%d, bright=%d, repeat=%d\n",
        (unsigned long)GetCurrentProcessId(), o.K, files.size(), o.dark, o.bright, o.repeat);
    std::fflush(stdout);

    ULONGLONG t0 = GetTickCount64();
    for (int i = 0; i < o.K; ++i) ResumeThread(h[i]);

    // Очікуємо завершення всіх потоків одночасно (як вимагає ТЗ)
    WaitForMultipleObjects(o.K, h.data(), TRUE, INFINITE);

    ULONGLONG elapsed = GetTickCount64() - t0;

    // Зчитуємо коди завершення потоків і закриваємо дескриптори
    DWORD totalThreadErrors = 0;
    for (int i = 0; i < o.K; ++i) {
        DWORD exitCode = 0;
        GetExitCodeThread(h[i], &exitCode);
        totalThreadErrors += exitCode; // exitCode містить кількість помилок у цьому потоці
        CloseHandle(h[i]);
    }
    // об'єднання результатів - тільки після завершення потоків
    std::vector<FileResult> all;
    for (int i = 0; i < o.K; ++i)
        for (auto& r : ctx[i].results) all.push_back(r);
    std::sort(all.begin(), all.end(), [](const FileResult& a, const FileResult& b) { return a.name < b.name; });

    std::ofstream sm(o.out / "summary.csv", std::ios::binary);
    std::ofstream er(o.out / "errors.csv", std::ios::binary);
    sm << "file;count;min;max;mean;median;dark_fraction;bright_fraction\n";
    er << "file;error\n";
    int bad = 0;
    for (auto& r : all) {
        if (r.ok) {
            unsigned long long s = 0;
            for (int i = 0; i < 256; ++i) s += r.hist[i];
            if (s != (unsigned long long)r.width * (unsigned long long)r.height) {   // контроль суми частот
                r.ok = false; r.error = "сума частот != width*height";
            }
        }
        if (r.ok) {
            sm << r.name << ';' << r.count << ';' << r.minV << ';' << r.maxV << ';' << Fmt(r.mean) << ';'
                << Fmt(r.median) << ';' << Fmt(r.darkFrac) << ';' << Fmt(r.brightFrac) << '\n';
        }
        else {
            er << r.name << ';' << r.error << '\n';
            std::fprintf(stderr, "ПОМИЛКА %s: %s\n", r.name.c_str(), r.error.c_str());
            ++bad;
        }
    }
    std::printf("Готово: оброблено %zu файлів, помилок %d, час %llu мс. Результати: %s\n",
        all.size(), bad, (unsigned long long)elapsed, o.out.string().c_str());
    std::system("pause");
    return bad ? 1 : 0;
}