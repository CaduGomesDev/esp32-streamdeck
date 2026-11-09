#include "Config.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace {

struct KeyEntry {
    const char *name;
    uint8_t code;
};

// So expomos F13-F24: nao existem em teclados fisicos comuns, entao nao
// conflitam com atalhos do sistema (ver README).
const KeyEntry kKeyTable[] = {
    {"F13", 0xF0}, {"F14", 0xF1}, {"F15", 0xF2}, {"F16", 0xF3},
    {"F17", 0xF4}, {"F18", 0xF5}, {"F19", 0xF6}, {"F20", 0xF7},
    {"F21", 0xF8}, {"F22", 0xF9}, {"F23", 0xFA}, {"F24", 0xFB},
};
const int kKeyTableLen = sizeof(kKeyTable) / sizeof(kKeyTable[0]);

void setStr(char *dst, size_t dstSize, const char *src) {
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

bool isValidType(const char *type) {
    return strcmp(type, "key") == 0 || strcmp(type, "macro") == 0 ||
           strcmp(type, "volume_up") == 0 || strcmp(type, "volume_down") == 0 ||
           strcmp(type, "volume_mute") == 0 || strcmp(type, "status") == 0 ||
           strcmp(type, "screensaver") == 0 || strcmp(type, "notifications") == 0 ||
           strcmp(type, "usage") == 0;
}

} // namespace

namespace Config {

void loadDefaults(DeckConfig &cfg) {
    static const char *labels[] = {"F13", "F14", "F15", "F16", "F17", "F18"};
    static const char *keys[] = {"F13", "F14", "F15", "F16", "F17", "F18"};
    static const char *colors[] = {"#FF0000", "#FFA500", "#FFFF00",
                                    "#00FF00", "#00FFFF", "#0000FF"};

    cfg.buttonCount = 6;
    cfg.idleTimeoutMs = 30000;
    cfg.boxOpacity = DEFAULT_BOX_OPACITY;
    for (int i = 0; i < 6; i++) {
        ButtonConfig &b = cfg.buttons[i];
        setStr(b.label, sizeof(b.label), labels[i]);
        setStr(b.type, sizeof(b.type), "key");
        setStr(b.key, sizeof(b.key), keys[i]);
        b.macroText[0] = '\0';
        b.macroWinR = false;
        b.macroEnter = false;
        setStr(b.colorHex, sizeof(b.colorHex), colors[i]);
    }
    for (int i = 6; i < MAX_BUTTONS; i++) {
        ButtonConfig &b = cfg.buttons[i];
        b.label[0] = '\0';
        b.type[0] = '\0';
        b.key[0] = '\0';
        b.macroText[0] = '\0';
        b.macroWinR = false;
        b.macroEnter = false;
        b.colorHex[0] = '\0';
    }
}

bool load(DeckConfig &cfg) {
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
        loadDefaults(cfg);
        save(cfg);
        return false;
    }

    String contents = f.readString();
    f.close();

    if (!fromJson(contents.c_str(), cfg)) {
        loadDefaults(cfg);
        save(cfg);
        return false;
    }
    return true;
}

bool save(const DeckConfig &cfg) {
    char buf[2048];
    size_t len = toJson(cfg, buf, sizeof(buf));
    if (len == 0) return false;

    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f) return false;
    size_t written = f.print(buf);
    f.close();
    return written == len;
}

size_t toJson(const DeckConfig &cfg, char *out, size_t outSize) {
    JsonDocument doc;
    doc["idle_timeout_ms"] = cfg.idleTimeoutMs;
    doc["box_opacity"] = cfg.boxOpacity;
    JsonArray arr = doc["buttons"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg.buttonCount && i < MAX_BUTTONS; i++) {
        const ButtonConfig &b = cfg.buttons[i];
        JsonObject o = arr.add<JsonObject>();
        o["label"] = b.label;
        o["type"] = b.type;
        o["key"] = b.key;
        o["macro_text"] = b.macroText;
        o["macro_winr"] = b.macroWinR;
        o["macro_enter"] = b.macroEnter;
        o["color"] = b.colorHex;
        o["has_icon"] = LittleFS.exists(iconPath(i));
    }
    size_t len = serializeJson(doc, out, outSize);
    return len;
}

bool fromJson(const char *json, DeckConfig &cfg) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;

    JsonArrayConst arr = doc["buttons"].as<JsonArrayConst>();
    if (arr.isNull() || arr.size() == 0) return false;

    uint8_t count = 0;
    for (JsonObjectConst o : arr) {
        if (count >= MAX_BUTTONS) break;

        const char *label = o["label"] | "";
        const char *type = o["type"] | "key";
        const char *key = o["key"] | "";
        const char *macroText = o["macro_text"] | "";
        bool macroWinR = o["macro_winr"] | false;
        bool macroEnter = o["macro_enter"] | false;
        const char *color = o["color"] | "#FFFFFF";

        if (!isValidType(type)) continue;
        if (strcmp(type, "key") == 0 && keyNameToCode(key) == 0) continue;
        if (strcmp(type, "macro") == 0 && macroText[0] == '\0') continue;

        ButtonConfig &b = cfg.buttons[count];
        setStr(b.label, sizeof(b.label), label);
        setStr(b.type, sizeof(b.type), type);
        setStr(b.key, sizeof(b.key), key);
        setStr(b.macroText, sizeof(b.macroText), macroText);
        b.macroWinR = macroWinR;
        b.macroEnter = macroEnter;
        setStr(b.colorHex, sizeof(b.colorHex), color);
        count++;
    }
    if (count == 0) return false;

    cfg.buttonCount = count;
    cfg.idleTimeoutMs = doc["idle_timeout_ms"] | 30000;
    int opacity = doc["box_opacity"] | DEFAULT_BOX_OPACITY;
    cfg.boxOpacity = (uint8_t)constrain(opacity, 0, 100);
    return true;
}

uint8_t keyNameToCode(const char *name) {
    for (int i = 0; i < kKeyTableLen; i++) {
        if (strcmp(kKeyTable[i].name, name) == 0) return kKeyTable[i].code;
    }
    return 0;
}

const char *keyCodeToName(uint8_t code) {
    for (int i = 0; i < kKeyTableLen; i++) {
        if (kKeyTable[i].code == code) return kKeyTable[i].name;
    }
    return "";
}

uint16_t colorHexToRGB565(const char *hex) {
    if (hex[0] != '#' || strlen(hex) < 7) return 0xFFFF; // branco por default
    auto hexPair = [](const char *p) -> uint8_t {
        char buf[3] = {p[0], p[1], '\0'};
        return (uint8_t)strtol(buf, nullptr, 16);
    };
    uint8_t r = hexPair(hex + 1);
    uint8_t g = hexPair(hex + 3);
    uint8_t b = hexPair(hex + 5);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

String iconPath(uint8_t index) {
    return "/icon" + String(index) + ".rgba";
}

} // namespace Config
