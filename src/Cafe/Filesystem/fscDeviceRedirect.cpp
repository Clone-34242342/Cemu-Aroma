#include "util/helpers/helpers.h"
#include "Cafe/Filesystem/fscDeviceHostFS.h"
#include "FST/fstUtil.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <atomic>
#include <future>
#include <map>
#include <cmath>
#include <wx/image.h>
#include <wx/mstream.h>

struct RedirectEntry
{
	RedirectEntry(const fs::path& dstPath, sint32 priority) : dstPath(dstPath), priority(priority) {}
	fs::path dstPath;
	sint32 priority;
};

FSAFileTree<RedirectEntry> redirectTree;

void fscDeviceRedirect_add(std::string_view virtualSourcePath, size_t fileSize, const fs::path& targetFilePath, sint32 priority)
{
	// check if source already has a redirection
	RedirectEntry* existingEntry;
	if (redirectTree.getFile(virtualSourcePath, existingEntry))
	{
		if (existingEntry->priority >= priority)
			return; // dont replace entries with equal or higher priority
		// unregister existing entry
		redirectTree.removeFile(virtualSourcePath);
		delete existingEntry;
	}
	RedirectEntry* entry = new RedirectEntry(targetFilePath, priority);
	redirectTree.addFile(virtualSourcePath, fileSize, entry);
}

namespace
{
class WsuBridgeFile final : public FSCVirtualFile
{
public:
    explicit WsuBridgeFile(std::string content) : m_content(std::move(content)) {}
    sint32 fscGetType() override { return FSC_TYPE_FILE; }
    uint64 fscQueryValueU64(uint32 id) override { return id == FSC_QUERY_SIZE ? m_content.size() : 0; }
    uint32 fscReadData(void* dst, uint32 size) override
    {
        if (m_seek >= m_content.size()) return 0;
        size = (uint32)std::min<uint64>(size, m_content.size() - m_seek);
        std::memcpy(dst, m_content.data() + m_seek, size);
        m_seek += size;
        return size;
    }
    void fscSetSeek(uint64 seek) override { m_seek = seek; }
    uint64 fscGetSeek() override { return m_seek; }
private:
    std::string m_content;
    uint64 m_seek = 0;
};

std::string DownloadWsuFile(const std::string& url, long timeout, long connectTimeout, bool local, const std::string& post = {}, long* httpStatus = nullptr)
{
    std::string response;
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (local)
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
    else
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "WiiStreetU-Revival/0.4.15 (+https://github.com/Littlemac123/WiiStreetU-Revival)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connectTimeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_slist* headers = nullptr;
    if (!post.empty())
    {
        headers = curl_slist_append(headers, "Content-Type: application/json+protobuf");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post.size()));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* data, size_t unit, size_t count, void* arg) -> size_t
        {
            auto& buffer = *static_cast<std::string*>(arg);
            const size_t len = unit * count;
            if (len > 5 * 1024 * 1024 - buffer.size()) return 0;
            buffer.append(data, len);
            return len;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const auto result = curl_easy_perform(curl);
    if (httpStatus) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpStatus);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return result == CURLE_OK ? std::move(response) : std::string{};
}

std::string WsuMapUrl(std::string_view key)
{
    char provider;
    unsigned z, x, y;
    int end = 0;
    if (std::sscanf(std::string(key).c_str(), "%c~%u~%u~%u%n", &provider, &z, &x, &y, &end) != 4 ||
        static_cast<size_t>(end) != key.size() || z > (provider == 'g' ? 20u : 19u) || x >= (1u << z) || y >= (1u << z))
        return {};
    switch (provider)
    {
    case 'g':
        return fmt::format(
                "https://mts.googleapis.com/vt?pb=!1m4!1m3!1i{}!2i{}!3i{}"
                "!2m8!1e2!2ssvv!4m2!1scc!2s*211m3*211e2*212b1*213e2*211m3*211e3*212b1*213e2"
                "*211m3*211e10*212b1*213e2*212b1*214b1!4m2!1ssvl!2s*211b1*212b1"
                "!3m11!2sen!3sUS!12m4!1e68!2m2!1sset!2sRoadmap!12m3!1e37!2m1!1ssmartmaps!5m1!5f1", z, x, y);
    case 'o': return fmt::format("https://tile.openstreetmap.org/{}/{}/{}.png", z, x, y);
    case 'e':
        if (z <= 14)
            return fmt::format("https://tiles.maps.eox.at/wmts/1.0.0/s2cloudless-2025_3857/default/g/{}/{}/{}.jpg", z, y, x);
        break;
    case 'm': return fmt::format("https://imagery.michigan.gov/server/rest/services/Michigan_imagery_public/MapServer/tile/{}/{}/{}?blankTile=false", z, y, x);
    case 'u':
        if (z <= 16)
            return fmt::format("https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/{}/{}/{}?blankTile=false", z, y, x);
        break;
    }
    return {};
}

struct WsuRequest
{
    std::string url;
    std::string post;
    bool image = false;
    std::string coverageFallback;
    bool coverage = false;
};

struct WsuStaticMap
{
    unsigned z, x, y, nx, ny, offsetX, offsetY, width, height;
    char layer;
};

bool ParseWsuStaticMap(const std::string& args, WsuStaticMap& map)
{
    int end = 0;
    return std::sscanf(args.c_str(), "%u~%u~%u~%u~%u~%c~%u~%u~%u~%u%n",
        &map.z, &map.x, &map.y, &map.nx, &map.ny, &map.layer,
        &map.offsetX, &map.offsetY, &map.width, &map.height, &end) == 10 &&
        end == args.size() && map.z <= 20 && map.x < (1u << map.z) && map.y < (1u << map.z) &&
        map.nx > 0 && map.nx <= 6 && map.ny > 0 && map.ny <= 6 &&
        (map.layer == 'm' || map.layer == 'p') && map.offsetX < 256 && map.offsetY < 256 &&
        map.width > 0 && map.width <= 1024 && map.height > 0 && map.height <= 1024 &&
        map.width + map.offsetX <= map.nx * 256 && map.height + map.offsetY <= map.ny * 256;
}

std::string ExportWsuStaticMap(const std::string& args)
{
    WsuStaticMap map{};
    if (!ParseWsuStaticMap(args, map)) return {};
    wxImage output(map.width, map.height, false);
    output.SetRGB(wxRect(0, 0, map.width, map.height), 216, 232, 231);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(9);
    unsigned loaded = 0;
    for (unsigned row = 0; row < map.ny; ++row)
        for (unsigned column = 0; column < map.nx; ++column)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return {};
            const auto zoom = std::min(map.z, 19u), scale = 1u << (map.z - zoom);
            const auto x = (map.x + column) % (1u << map.z), y = map.y + row;
            if (y >= (1u << map.z)) continue;
            const auto url = WsuMapUrl(fmt::format("o~{}~{}~{}", zoom, x / scale, y / scale));
            const auto bytes = DownloadWsuFile(url, std::min<long>(remaining, 2500L), 1500L, false);
            if (bytes.empty()) continue;
            wxMemoryInputStream input(bytes.data(), bytes.size());
            wxImage tile(input, wxBITMAP_TYPE_PNG);
            if (!tile.IsOk() || tile.GetWidth() != 256 || tile.GetHeight() != 256) continue;
            if (scale > 1)
                tile = tile.GetSubImage(wxRect((x % scale) * 256 / scale, (y % scale) * 256 / scale,
                    256 / scale, 256 / scale)).Scale(256, 256, wxIMAGE_QUALITY_NORMAL);
            output.Paste(tile, static_cast<int>(column * 256) - static_cast<int>(map.offsetX),
                static_cast<int>(row * 256) - static_cast<int>(map.offsetY));
            ++loaded;
        }
    if (!loaded) return {};
    output.SetOption(wxIMAGE_OPTION_QUALITY, 85);
    wxMemoryOutputStream stream;
    if (!output.SaveFile(stream, wxBITMAP_TYPE_JPEG)) return {};
    std::string bytes(stream.GetSize(), '\0');
    stream.CopyTo(bytes.data(), bytes.size());
    return bytes;
}

WsuRequest WsuProviderRequest(std::string_view route, std::string_view arguments)
{
    const std::string args(arguments);
    if (args.empty() || args.size() > 190) return {};
    if (route == "static")
    {
        WsuStaticMap map{};
        if (!ParseWsuStaticMap(args, map)) return {};
        return {"wsu-static:" + args, {}, true};
    }
    if (route == "map")
    {
        WsuRequest request{WsuMapUrl(args), {}, true};
        request.coverage = args.starts_with("g~");
        unsigned x, y;
        if (!request.url.empty() && args.starts_with("g~17~") &&
            std::sscanf(args.c_str(), "g~17~%u~%u", &x, &y) == 2)
            request.coverageFallback = fmt::format(
                "https://www.google.com/maps/photometa/ac/v1?pb=!1m1!1smaps_sv.tactile!6m3!1i{}!2i{}!3i17!8b1", x, y);
        return request;
    }
    if (route == "meta" && args.starts_with("id~"))
    {
        const auto id = args.substr(3);
        if (id.empty() || id.size() > 96 || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != id.npos) return {};
        return {fmt::format("https://www.google.com/maps/photometa/v1?authuser=0&hl=en&gl=en&pb="
            "!1m4!1smaps_sv.tactile!11m2!2m1!1b1!2m2!1sen!2sen!3m3!1m2!1e2!2s{}"
            "!4m54!1e1!1e2!1e3!1e4!1e5!1e6!1e8!1e12!2m1!1e1!4m1!1i48!5m0!6m0!9m36"
            "!1m3!1e2!2b1!3e2!1m3!1e2!2b0!3e3!1m3!1e3!2b1!3e2!1m3!1e3!2b0!3e3"
            "!1m3!1e8!2b0!3e3!1m3!1e1!2b0!3e3!1m3!1e4!2b0!3e3!1m3!1e10!2b1!3e2"
            "!1m3!1e10!2b0!3e3!11m2!3m1!4b1", id)};
    }
    if (route == "thumb" || route == "tile" || route == "step")
    {
        char id[97];
        unsigned first, second, third;
        int yaw, pitch, end = 0;
        if ((route == "tile" || route == "step") &&
            std::sscanf(args.c_str(), "%96[A-Za-z0-9_-]~%u~%u~%u%n", id, &first, &second, &third, &end) == 4 &&
            end == args.size() && first <= 5 && second <= 31 && third <= 15)
            return {fmt::format("https://streetviewpixels-pa.googleapis.com/v1/tile?cb_client=maps_sv.tactile&panoid={}&zoom={}&x={}&y={}", id, first, second, third), {}, true};
        if (route == "thumb" &&
            std::sscanf(args.c_str(), "%96[A-Za-z0-9_-]~%u~%u~%d~%d%n", id, &first, &second, &yaw, &pitch, &end) == 5 &&
            end == args.size() && first >= 16 && first <= 340 && second >= 16 && second <= 251 &&
            yaw >= -360 && yaw <= 360 && pitch >= -90 && pitch <= 90)
            return {fmt::format("https://streetviewpixels-pa.googleapis.com/v1/thumbnail?cb_client=maps_sv.tactile&w={}&h={}&pitch={}&panoid={}&yaw={}", first, second, pitch, id, yaw), {}, true};
        return {};
    }
    if (route == "search")
    {
        if (args.size() > 186 || args.size() % 2 || args.find_first_not_of("0123456789abcdef") != args.npos) return {};
        std::string query;
        for (size_t i = 0; i < args.size(); i += 2) query += "%" + args.substr(i, 2);
        return {"https://photon.komoot.io/api/?limit=5&q=" + query};
    }
    if (route == "route")
    {
        if (args.size() < 3 || (args[0] != 'c' && args[0] != 'f') || args[1] != '~') return {};
        std::string points;
        size_t offset = 2;
        unsigned count = 0;
        while (offset < args.size())
        {
            const auto separator = args.find('~', offset);
            const auto point = args.substr(offset, separator == args.npos ? args.npos : separator - offset);
            double latitude, longitude;
            int end = 0;
            if (++count > 5 || std::sscanf(point.c_str(), "%lf,%lf%n", &latitude, &longitude, &end) != 2 ||
                end != point.size() || !std::isfinite(latitude) || !std::isfinite(longitude) ||
                std::abs(latitude) > 90 || std::abs(longitude) > 180)
                return {};
            points += fmt::format("{}{:.6f},{:.6f}", points.empty() ? "" : ";", longitude, latitude);
            if (separator == args.npos) break;
            offset = separator + 1;
        }
        if (count < 2 || args.back() == '~') return {};
        return {fmt::format("https://routing.openstreetmap.de/routed-{}/route/v1/driving/{}"
            "?overview=full&geometries=geojson&steps=false&generate_hints=false&alternatives=true",
            args[0] == 'c' ? "car" : "foot", points)};
    }
    double lat, lng;
    unsigned radius;
    int end = 0;
    const bool sphere = args.starts_with("sphere~");
    const unsigned type = sphere ? 10 : 2;
    if (route == "meta" &&
        std::sscanf(args.c_str(), sphere ? "sphere~%lf~%lf~%u%n" : "coords~%lf~%lf~%u%n", &lat, &lng, &radius, &end) == 3 &&
        end == args.size() && std::isfinite(lat) && std::isfinite(lng) &&
        std::abs(lat) <= 90 && std::abs(lng) <= 180 && radius >= 1 && radius <= (sphere ? 8u : 1000000u))
        return {"https://maps.googleapis.com/$rpc/google.internal.maps.mapsjs.v1.MapsJsInternalService/SingleImageSearch",
            fmt::format("[[\"apiv3\",null,null,null,\"US\"],[[null,null,{:.6f},{:.6f}],{}],"
                "[null,[\"en\",\"en\"],null,null,null,null,null,null,[{}],null,[[[{},true,2]]]],"
                "[[1,2,3,4,6,8,12],null,null,null,[],[]]]", lat, lng, radius, type, type)};
    if ((route != "weather" && route != "elev") ||
        std::sscanf(args.c_str(), "%lf~%lf%n", &lat, &lng, &end) != 2 || end != args.size() ||
        !std::isfinite(lat) || !std::isfinite(lng) || std::abs(lat) > 90 || std::abs(lng) > 180)
        return {};
    if (route == "elev")
        return {fmt::format("https://api.open-meteo.com/v1/elevation?latitude={:.3f}&longitude={:.3f}", lat, lng)};
    return {fmt::format("https://api.open-meteo.com/v1/forecast?latitude={:.2f}&longitude={:.2f}"
        "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=5&timezone=auto", lat, lng)};
}

FSCVirtualFile* OpenWsuService(std::string_view path, sint32& status)
{
    constexpr std::string_view prefix = "vol/content/app/HexagonRe_for_1.0j/wsu-direct/";
    if (!path.starts_with(prefix)) return nullptr;
    path.remove_prefix(prefix.size());
    status = FSC_STATUS_OK;
    if (path == "settings.historical-events.txt" || path == "settings.coverage.txt" ||
        path == "settings.step-priority.txt") return new WsuBridgeFile("on");
    if (path == "settings.pre-v96-google.txt") return new WsuBridgeFile(std::getenv("CEMU_WII_STREET_U_PRE_V96") ? "on" : "no");
    if (path == "settings.auto-enter.txt") return new WsuBridgeFile(std::getenv("CEMU_WII_STREET_U_AUTO_ENTER") ? "on" : "no");
    if (path == "settings.original-menus.txt") return new WsuBridgeFile(std::getenv("CEMU_WII_STREET_U_RESTORED_MENUS") ? "no" : "on");
    if (path.starts_with("settings.panorama-detail.") && path.ends_with(".txt"))
    {
        const auto detail = [](const char* key, char fallback) {
            const char* value = std::getenv(key);
            return value && value[0] >= '0' && value[0] <= '5' && !value[1] ? value[0] : fallback;
        };
        std::string settings{
            detail("CEMU_WII_STREET_U_INITIAL_DETAIL", '2'),
            detail("CEMU_WII_STREET_U_SHARPEN_TO", '3'),
            std::getenv("CEMU_WII_STREET_U_HIDE_TV_COVERAGE") ? '0' : '1',
            std::getenv("CEMU_WII_STREET_U_HIDE_TV_MAP") ? '0' : '1',
            std::getenv("CEMU_WII_STREET_U_GRADUAL_SHARPENING") ? '1' : '0'};
        return new WsuBridgeFile(std::move(settings));
    }
    if (path == "settings.native-map-export.txt") return new WsuBridgeFile("on");
    if (path.starts_with("trace.") && path.ends_with(".txt"))
    {
        const bool tracing = std::getenv("CEMU_WII_STREET_U_TRACE") != nullptr;
        if (path.starts_with("trace.status.")) return new WsuBridgeFile(tracing ? "on" : "no");
        if (tracing && path.size() <= 240)
            cemuLog_log(LogType::Force, "[WSU Trace] {}", path.substr(6, path.size() - 10));
        return new WsuBridgeFile(tracing ? "ok" : "no");
    }
    if (!path.ends_with(".js")) return nullptr;
    path.remove_suffix(3);
    const auto routeEnd = path.find('.');
    if (routeEnd == path.npos || routeEnd > 7) return nullptr;
    const auto route = path.substr(0, routeEnd);
    path.remove_prefix(routeEnd + 1);
    auto dot = path.find('.');
    if (dot == path.npos) return nullptr;
    const auto callback = path.substr(0, dot);
    if (!callback.starts_with("wsud") || callback.size() <= 4 || callback.size() > 15 ||
        callback.substr(4).find_first_not_of("0123456789") != path.npos) return nullptr;
    path.remove_prefix(dot + 1);
    dot = path.find('.');
    if (dot == path.npos) return nullptr;
    const auto arguments = path.substr(dot + 1);
    const auto request = WsuProviderRequest(route, arguments);
    if (request.url.empty()) return new WsuBridgeFile(fmt::format("{}(null);", callback));
    const auto key = std::string(route) + "/" + std::string(callback) + "/" + std::string(arguments);

    // Use the same pending response as the Aroma plugin while the network request runs.
    static std::mutex mutex;
    static std::map<std::string, std::shared_future<std::string>, std::less<>> tiles;
    std::lock_guard lock(mutex);
    auto tile = tiles.find(key);
    auto ready = [](const auto& entry) { return entry.second.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
    if (tile == tiles.end())
    {
        if (tiles.size() >= 16)
        {
            auto finished = std::find_if(tiles.begin(), tiles.end(), ready);
            if (finished == tiles.end()) return nullptr;
            tiles.erase(finished);
        }
        tile = tiles.emplace(key, std::async(std::launch::async, [request] {
            using Clock = std::chrono::steady_clock;
            static std::atomic<Clock::duration::rep> coverageRetry{0};
            std::string bytes;
            if (!request.coverage || Clock::now().time_since_epoch().count() >= coverageRetry.load())
            {
                long status = 0;
                bytes = request.url.starts_with("wsu-static:") ? ExportWsuStaticMap(request.url.substr(11)) :
                    DownloadWsuFile(request.url, 7000L, 2500L, false, request.post, &status);
                if (request.coverage && (status == 403 || status == 429))
                    coverageRetry.store((Clock::now() + std::chrono::minutes(1)).time_since_epoch().count());
            }
            const bool validImage = bytes.starts_with("\x89PNG\r\n\x1a\n") || bytes.starts_with("\xff\xd8\xff");
            if (!validImage && !request.coverageFallback.empty())
            {
                bytes = DownloadWsuFile(request.coverageFallback, 3000L, 1500L, false);
                if (!bytes.starts_with(")]}'")) return std::string{};
            }
            else if (bytes.empty() || (request.image && !validImage))
                return std::string{};
            std::string encoded(4 * ((bytes.size() + 2) / 3) + 1, '\0');
            encoded.resize(EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size())));
            return encoded;
        }).share()).first;
    }
    status = FSC_STATUS_OK;
    if (!ready(*tile)) return new WsuBridgeFile(fmt::format("{}(false);", callback));
    const auto& encoded = tile->second.get();
    if (encoded.empty())
        return new WsuBridgeFile(fmt::format("{}(null);", callback));
    return new WsuBridgeFile(fmt::format("{}(\"{}\");", callback, encoded));
}

FSCVirtualFile* OpenWsuBridge(std::string_view path, sint32& status)
{
    // Opt-in to this adapter for this one title. Never proxy arbitrary URLs.
    if (CafeSystem::GetForegroundTitleId() != 0x0005000010132000ull) return nullptr;
    if (path.starts_with('/')) path.remove_prefix(1);
    // The keyboard buffer is emulator state even when Aroma supplies the map service.
    constexpr std::string_view keyboardSettings = "vol/content/app/HexagonRe_for_1.0j/cemu-keyboard/";
    if (path.starts_with(keyboardSettings))
    {
        const auto setting = path.substr(keyboardSettings.size());
        const auto serial = setting.starts_with("settings.submitted-text.") && setting.ends_with(".txt")
            ? setting.substr(24, setting.size() - 28) : std::string_view{};
        if (setting == "settings.keyboard.txt" ||
            (!serial.empty() && serial.find_first_not_of("0123456789") == std::string_view::npos))
        {
            status = FSC_STATUS_OK;
            return new WsuBridgeFile(setting == "settings.keyboard.txt" ? "on" : swkbd::GetSubmittedTextAsJson());
        }
    }
    if (std::getenv("CEMU_WII_STREET_U_MAPS"))
        if (auto* service = OpenWsuService(path, status)) return service;
    constexpr std::string_view prefix = "vol/content/app/HexagonRe_for_1.0j/bridge-";
    if (!std::getenv("CEMU_WII_STREET_U_BRIDGE") || !path.starts_with(prefix) || !path.ends_with(".js")) return nullptr;
    const auto id = path.substr(prefix.size(), path.size() - prefix.size() - 3);
    if (id.empty() || id.size() > 180 ||
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string_view::npos)
        return nullptr;

    constexpr std::string_view keyboardPrefix = "keyboard.wsu";
    if (id.starts_with(keyboardPrefix))
    {
        const auto digits = id.substr(keyboardPrefix.size());
        if (digits.empty() || digits.size() > 9 || digits.find_first_not_of("0123456789") != std::string_view::npos)
            return nullptr;
        status = FSC_STATUS_OK;
        return new WsuBridgeFile(fmt::format("wsu{}({});", digits, swkbd::GetSubmittedTextAsJson()));
    }

    auto response = DownloadWsuFile("http://127.0.0.1:8765/file/" + std::string(id), 12000L, 400L, true);
    if (response.empty()) return nullptr;
    status = FSC_STATUS_OK;
    return new WsuBridgeFile(std::move(response));
}
}

class fscDeviceTypeRedirect : public fscDeviceC
{
	FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
	{
        if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) &&
            !HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION))
        {
            if (auto* bridge = OpenWsuBridge(path, *fscStatus)) return bridge;
        }

		RedirectEntry* redirectionEntry;

		if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) && redirectTree.getFile(path, redirectionEntry))
			return FSCVirtualFile_Host::OpenFile(redirectionEntry->dstPath, accessFlags, *fscStatus);

		FSCVirtualFile* dirIterator;

		if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR) && redirectTree.getDirectory(path, dirIterator))
			return dirIterator;

		return nullptr;
	}

public:
	static fscDeviceTypeRedirect& instance()
	{
		static fscDeviceTypeRedirect _instance;
		return _instance;
	}
};

bool _redirectMapped = false;

void fscDeviceRedirect_map()
{
	if (_redirectMapped)
		return;
	fsc_mount("/", "/", &fscDeviceTypeRedirect::instance(), nullptr, FSC_PRIORITY_REDIRECT);
	_redirectMapped = true;
}
