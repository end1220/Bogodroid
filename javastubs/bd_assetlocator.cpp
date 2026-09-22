// com.mobge.assetlocator.AssetLocator — Oddmar's own Java helper over the APK
// asset tree.
//
// The port does not run the APK's DEX, so Oddmar's Java-side asset walker has to
// be answered from the staged filesystem. The loader chdir()s into
// paths.game_files (gamedata/), and the APK's assets are staged at
// gamedata/assets/, so the root of the asset tree is "assets" relative to the
// process cwd — the same assumption android.content.res.AssetManager::list()
// already makes.
//
// Why this matters for the picture: MobGe.Storage.AndroidAssetManager.Create()
// calls GetRelativeFilePaths(), which calls AssetLocator.ListAssets() and
// iterates the returned String[]. With no AssetLocator class registered, jnivm
// minted an empty auto-stub, the reflected ListAssets lookup returned null, the
// managed array came back null, and GetRelativeFilePaths threw
// NullReferenceException during startup — so the game never located a single
// asset and Unity kept presenting black frames.
//
// Semantics mirror AssetManager.list(): the *child names* of one directory, not
// full paths. The managed caller re-prefixes the directory itself while
// recursing, so returning full paths here would double the directory component.

#include "toml++/toml.hpp"
extern toml::table config;

#include "android.h"
#include "baron/baron.h"
#include "javac.h"
#include "unity.h"
#include "logging.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// The directory AssetLocator paths are resolved against: "assets" under the
// process cwd, falling back to <paths.game_files>/assets if this ever runs with
// a different cwd.
std::filesystem::path bd_asset_root()
{
    std::error_code ec;
    std::filesystem::path local("assets");
    if (std::filesystem::is_directory(local, ec))
        return local;

    std::filesystem::path gameFiles(
        config["paths"]["game_files"].value_or<std::string>("."));
    gameFiles /= "assets";
    return gameFiles;
}

// Asset paths arrive from the managed side as "Bundles", "Bundles/x", sometimes
// with a leading "./" or "/". std::filesystem::path::operator/ *replaces* the
// whole path when the right-hand side is absolute, so a single leading slash
// would silently escape the asset root and make every listing come back empty.
std::filesystem::path bd_relative(const std::string& raw)
{
    std::string s = raw;
    while (!s.empty() && (s.front() == '/' || s.front() == '\\'))
        s.erase(s.begin());
    while (s.rfind("./", 0) == 0)
        s.erase(0, 2);
    return std::filesystem::path(s).lexically_normal();
}

} // namespace

jnivm::com::mobge::assetlocator::AssetLocator::AssetLocator(
    std::shared_ptr<FakeJni::JObject> context,
    std::shared_ptr<FakeJni::JString> root)
{
    const std::filesystem::path base = bd_asset_root();
    mRoot = base.string();

    // The second constructor argument is the root the managed side believes in.
    // Treat it as a subdirectory of the asset tree when it actually resolves to
    // one; otherwise keep the plain asset root so a bare name (or an empty
    // string) cannot send every lookup into a directory that does not exist.
    if (root != nullptr) {
        const std::string given = root->asStdString();
        if (!given.empty()) {
            std::error_code ec;
            std::filesystem::path candidate = base / bd_relative(given);
            if (std::filesystem::is_directory(candidate, ec))
                mRoot = candidate.string();
            BD_LOG("ASSETLOC", "AssetLocator root arg '%s' -> %s", given.c_str(),
                   mRoot.c_str());
        }
    }

    BD_LOG("ASSETLOC", "AssetLocator(context=%p) root=%s",
           (void*)context.get(), mRoot.c_str());
}

std::shared_ptr<jnivm::Array<FakeJni::JString>>
jnivm::com::mobge::assetlocator::AssetLocator::ListAssets(
    std::shared_ptr<FakeJni::JString> path)
{
    const std::string rel = path ? path->asStdString() : std::string();
    const std::filesystem::path dir = rel.empty()
        ? std::filesystem::path(mRoot)
        : std::filesystem::path(mRoot) / bd_relative(rel);

    std::vector<std::string> names;
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            names.push_back(entry.path().filename().string());
        }
    }

    BD_LOG("ASSETLOC", "ListAssets('%s') -> %zu entries from %s",
           rel.c_str(), names.size(), dir.string().c_str());

    auto result = std::make_shared<jnivm::Array<FakeJni::JString>>(names.size());
    for (size_t i = 0; i < names.size(); ++i)
        (*result)[i] = std::make_shared<FakeJni::JString>(names[i]);
    return result;
}

// MobGe.Storage.NativeAndroidReaderWrapper wraps the result in a
// System.IO.Stream and drives it with Close / Read / GetBytes / Seek /
// GetLength / GetPosition (IAssetReader's surface, see android.h).
//
// History, so nobody re-walks it:
//
//   v1  no method at all  -> lookup returned null  -> managed wrapper
//       dereferenced a null _javaObject inside Seek() -> every AssetBundle died
//       with "Unable to read header from archive file:" (x8).
//   v2  inert object      -> NRE gone and all eight bundles really opened
//       (bundle1 13,498,260 B ... bundle8 4,256,915 B), but every method
//       returned the stub default of 0, so Read() answered "0 bytes" and the
//       header still would not parse.
//   v3  this             -> a real reader over the staged file.
//
// The two signatures v2 proved are the ones the managed side asks for are
// Seek(JI)J and Read(I)I. Read() only reports how many bytes are available;
// GetBytes() is the half that hands over the payload.
std::shared_ptr<jnivm::Object>
jnivm::com::mobge::assetlocator::AssetLocator::GetReaderWrapper(
    std::shared_ptr<FakeJni::JString> path)
{
    const std::string rel = path ? path->asStdString() : std::string();
    const std::filesystem::path file = std::filesystem::path(mRoot) / bd_relative(rel);

    auto reader = std::make_shared<AssetReader>(file.string());
    BD_LOG("ASSETLOC", "GetReaderWrapper('%s') -> %s", rel.c_str(), file.string().c_str());
    return reader;
}

// ---------------------------------------------------------------------------
// com.mobge.assetlocator.AssetReader
// ---------------------------------------------------------------------------

namespace {

// Reads go through fseeko/fread on the aarch64 Linux target, where `long` is
// 64-bit (LP64) so offsets survive. Guard anyway so a 32-bit host build cannot
// silently truncate a >2 GB read offset.
int bd_seek(FILE* f, long long off)
{
#if defined(_WIN32)
    return _fseeki64(f, off, SEEK_SET);
#else
    return ::fseeko(f, (off_t)off, SEEK_SET);
#endif
}

} // namespace

jnivm::com::mobge::assetlocator::AssetReader::AssetReader(const std::string& path)
    : mPath(path)
{
    mFile = std::fopen(path.c_str(), "rb");
    if (mFile == nullptr) {
        BD_LOG("ASSETRD", "open FAILED %s (errno=%d)", path.c_str(), errno);
        return;
    }

    // Size comes from std::filesystem, not ftell(): the host stdio symbols can
    // be reached through the guest's thunk layer, and this is the same call the
    // [inert] probe already proved returns the real length (bundle1
    // 13498260 B). Measuring by seeking to SEEK_END is the other option, but it
    // costs a syscall pair and another chance to hit the wrong fseek.
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    mSize = ec ? 0 : (long long)size;
    mPos = 0;

    BD_LOG("ASSETRD", "open %s size=%lld", path.c_str(), mSize);
}

jnivm::com::mobge::assetlocator::AssetReader::~AssetReader()
{
    close_file();
}

void jnivm::com::mobge::assetlocator::AssetReader::close_file()
{
    if (mFile != nullptr) {
        std::fclose(mFile);
        mFile = nullptr;
    }
}

bool jnivm::com::mobge::assetlocator::AssetReader::trace_call(
    const char* what, long long a1, long long a2)
{
    const long long n = mCalls++;
    // Full trace for the opening moves (they reveal the real fetch pattern),
    // then every 64th call. Whole-session reader traffic measured in the
    // hundreds, so this stays cheap while showing where the cursor walks.
    const bool full = n < 64 || (n % 64) == 0;
    if (full) {
        BD_LOG("ASSETRD", "#%lld %s(%lld, %lld) pos=%lld/%lld pending=%zu",
               n, what, a1, a2, mPos, mSize, mPending.size());
    }
    return full;
}

jlong jnivm::com::mobge::assetlocator::AssetReader::Seek(jlong offset, jint origin)
{
    trace_call("Seek", (long long)offset, (long long)origin);
    if (mFile == nullptr)
        return mPos;

    long long base = 0;
    switch (origin) {
    case 1: base = mPos; break;  // SeekOrigin.Current
    case 2: base = mSize; break; // SeekOrigin.End
    default: base = 0; break;    // SeekOrigin.Begin
    }

    long long target = base + (long long)offset;
    if (target < 0) target = 0;
    // Past EOF is legal for a Stream; it just means the next Read returns 0.
    bd_seek(mFile, target);
    mPos = target;
    // Any measurement from the previous position is now stale.
    mPending.clear();
    return mPos;
}

jint jnivm::com::mobge::assetlocator::AssetReader::Read(jint count)
{
    trace_call("Read", (long long)count, 0);
    mPending.clear();
    if (mFile == nullptr || count <= 0)
        return 0;

    const long long want = (long long)count;
    const long long avail = mSize - mPos;
    const long long n = want < avail ? want : (avail > 0 ? avail : 0);
    if (n <= 0)
        return 0;

    // Measure now, hand over in GetBytes(). Doing the fread here costs one
    // extra copy but keeps GetBytes() free of a second cursor advance, so
    // "Read then GetBytes" and "GetBytes alone" both come out right.
    mPending.resize((size_t)n);
    // Drop any buffered stdio state: GetBytes() may be interleaved with a Seek
    // issued by a different layer, and re-seeking before every read is cheap
    // next to the page cache.
    bd_seek(mFile, mPos);
    const size_t got = std::fread(mPending.data(), 1, (size_t)n, mFile);
    mPending.resize(got);
    return (jint)got;
}

std::shared_ptr<FakeJni::JByteArray>
jnivm::com::mobge::assetlocator::AssetReader::GetBytes(jint count)
{
    trace_call("GetBytes", (long long)count, 0);

    if (!mPending.empty()) {
        // Payload Read() already measured. Charging the cursor here (and not in
        // Read) is what makes the pair advance exactly once — see GetBytesNoArg.
        std::vector<jbyte> bytes = std::move(mPending);
        mPending.clear();
        mPos += (long long)bytes.size();
        return std::make_shared<FakeJni::JByteArray>(bytes);
    }

    // Standalone GetBytes: read `count` bytes from the cursor and advance.
    if (mFile == nullptr || count <= 0)
        return std::make_shared<FakeJni::JByteArray>(std::vector<jbyte> {});

    long long n = (long long)count;
    if (n > mSize - mPos)
        n = mSize - mPos;
    if (n <= 0)
        return std::make_shared<FakeJni::JByteArray>(std::vector<jbyte> {});

    std::vector<jbyte> bytes((size_t)n);
    bd_seek(mFile, mPos);
    const size_t got = std::fread(bytes.data(), 1, (size_t)n, mFile);
    bytes.resize(got);
    mPos += (long long)got;
    return std::make_shared<FakeJni::JByteArray>(bytes);
}

std::shared_ptr<FakeJni::JByteArray>
jnivm::com::mobge::assetlocator::AssetReader::GetBytesNoArg()
{
    trace_call("GetBytes()", 0, 0);
    if (mPending.empty())
        return std::make_shared<FakeJni::JByteArray>(std::vector<jbyte> {});

    // This is the arity the managed wrapper actually uses (the run log shows
    // Read(n) -> GetBytes() pairs), so the cursor advance has to live here:
    // Read() only measures, and without this the stream never moves and every
    // block reads the same 32 KB from offset 0.
    std::vector<jbyte> bytes = std::move(mPending);
    mPending.clear();
    mPos += (long long)bytes.size();
    return std::make_shared<FakeJni::JByteArray>(bytes);
}

jlong jnivm::com::mobge::assetlocator::AssetReader::GetLength()
{
    trace_call("GetLength", 0, 0);
    return (jlong)mSize;
}

jlong jnivm::com::mobge::assetlocator::AssetReader::GetPosition()
{
    trace_call("GetPosition", 0, 0);
    return (jlong)mPos;
}

void jnivm::com::mobge::assetlocator::AssetReader::Close()
{
    BD_LOG("ASSETRD", "close %s after %lld calls, pos=%lld/%lld",
           mPath.c_str(), mCalls, mPos, mSize);
    mPending.clear();
    close_file();
}

// The game resolves the constructor through ReflectionHelper::getConstructorID()
// and then Constructor.newInstance(), so the signature it was asked for has to
// exist verbatim: jnivm rewrites `<init>(args)V` to a *static* `(args)L<class>;`
// and requires an exact match. Unity asks with whatever static type its
// AndroidJavaObject proxy was declared as, and that varies per call site —
// Ljava/lang/Object; on some paths, the concrete
// Lcom/unity3d/player/UnityPlayerActivity; on others. Register one entry per
// plausible first-argument type; they all funnel into the same C++ constructor.
BEGIN_NATIVE_DESCRIPTOR(jnivm::com::mobge::assetlocator::AssetLocator)
    { FakeJni::Constructor<AssetLocator,
                           std::shared_ptr<FakeJni::JObject>,
                           std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Constructor<AssetLocator,
                           std::shared_ptr<jnivm::android::content::Context>,
                           std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Constructor<AssetLocator,
                           std::shared_ptr<jnivm::android::app::Activity>,
                           std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Constructor<AssetLocator,
                           std::shared_ptr<jnivm::com::unity3d::player::UnityPlayerActivity>,
                           std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Function<&AssetLocator::ListAssets> {}, "ListAssets", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AssetLocator::GetReaderWrapper> {}, "GetReaderWrapper", FakeJni::JMethodID::PUBLIC },
END_NATIVE_DESCRIPTOR

// IAssetReader's surface. jnivm dispatches on name + signature, so the exact
// C++ parameter/return types below *are* the Java signatures the guest matches
// against — the two that the inert-stub run proved it asks for are Seek(JI)J
// and Read(I)I. GetBytes is registered twice because the literal pool does not
// say whether the managed side calls the 1-arg or the no-arg form; a miss on
// one costs nothing while it is the other that resolves.
BEGIN_NATIVE_DESCRIPTOR(jnivm::com::mobge::assetlocator::AssetReader)
    { FakeJni::Function<&AssetReader::Seek> {}, "Seek", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AssetReader::Read> {}, "Read", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AssetReader::GetBytes> {}, "GetBytes", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AssetReader::GetBytesNoArg> {}, "GetBytes", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AssetReader::GetLength> {}, "GetLength", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AssetReader::GetPosition> {}, "GetPosition", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AssetReader::Close> {}, "Close", FakeJni::JMethodID::PUBLIC },
END_NATIVE_DESCRIPTOR
