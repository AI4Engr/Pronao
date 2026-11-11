// The table of the application's own web files (see assets.h and app_native.h) and the files compiled into the program.
#include "assets.h"

#include "app_native.h"

std::shared_ptr<AssetStore> assetStore() {
    static std::shared_ptr<AssetStore> store = std::make_shared<AssetStore>();
    return store;
}

// provided by the file scripts/pack_assets.sh generates, or by its stub when there are none
int embeddedCount();
const char* embeddedName(int index);
const char* embeddedData(int index, unsigned long& size);

int app_asset_site(std::string origin, std::string csp) {
    assetStore()->configure(origin, csp);
    return 1;
}
int app_embedded_count() { return embeddedCount(); }
int app_embedded_name(int index, std::string& name) {
    if (index < 0 || index >= embeddedCount()) return 0;
    name = embeddedName(index);
    return 1;
}
int app_asset_add_embedded(std::string path, std::string mime, int index) {
    unsigned long size = 0;
    if (index < 0 || index >= embeddedCount()) return 0;
    const char* data = embeddedData(index, size);
    return assetStore()->add(path, mime, std::string(data, size)) ? 1 : 0;
}
int app_asset_add_text(std::string path, std::string mime, std::string text) { return assetStore()->add(path, mime, std::move(text)) ? 1 : 0; }
int app_asset_alias(std::string from, std::string to) { return assetStore()->alias(from, to) ? 1 : 0; }
