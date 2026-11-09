#include "SerialProtocol.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "Background.h"
#include "Icon.h"
#include "Notification.h"
#include "Screensaver.h"
#include "Status.h"
#include "Usage.h"

#define FW_VERSION "1.0.0"
#define LINE_BUF_SIZE 8192 // o notify leva o icone do app em base64 (~1,4 KB)
#define GIF_CHUNK_SIZE 1024
#define ACK_STEP 512 // bytes recebidos entre acks no upload (= bloco do host)
#define RECV_TIMEOUT_MS 60000
#define ICON_TMP_PATH "/icon_tmp.rgba"

namespace {

SerialProtocol::ApplyConfigFn gOnConfigApplied = nullptr;
SerialProtocol::AssetsChangedFn gOnAssetsChanged = nullptr;
SerialProtocol::NotifyFn gOnNotify = nullptr;
char gLineBuf[LINE_BUF_SIZE];
size_t gLineLen = 0;

void sendError(const char *code) {
    Serial.printf("{\"ok\":false,\"error\":\"%s\"}\n", code);
}

void handlePing() {
    Serial.printf("{\"ok\":true,\"fw\":\"%s\",\"max_buttons\":%d,\"max_gif_size\":%lu,\"icon_size\":%u}\n",
                  FW_VERSION, MAX_BUTTONS, (unsigned long)MAX_GIF_SIZE, (unsigned)Icon::BYTES);
}

void handleGetConfig() {
    DeckConfig cfg;
    Config::load(cfg);
    char cfgJson[2048];
    if (Config::toJson(cfg, cfgJson, sizeof(cfgJson)) == 0) {
        sendError("encode_failed");
        return;
    }
    Serial.print("{\"ok\":true,\"config\":");
    Serial.print(cfgJson);
    Serial.println("}");
}

void handleSetConfig(const char *line) {
    DeckConfig cfg;
    if (!Config::fromJson(line, cfg)) {
        sendError("invalid_config");
        return;
    }
    if (!Config::save(cfg)) {
        sendError("save_failed");
        return;
    }
    if (gOnConfigApplied) gOnConfigApplied(cfg);
    Serial.println("{\"ok\":true}");
}

// Le exatamente `size` bytes crus da serial (sem framing JSON) e grava em
// `tmpPath`. Confirma o recebimento a cada bloco lido (o host espera por
// isso antes de mandar o proximo — flow control simples, nunca mais de um
// bloco "em voo" pra nao estourar o buffer de recepcao do USB CDC). Em caso
// de sucesso retorna true (arquivo tmpPath pronto); em erro ja manda a
// resposta JSON de erro e retorna false.
bool receiveBytes(const String &tmpPath, uint32_t size) {
    File f = LittleFS.open(tmpPath, "w");
    if (!f) {
        sendError("fs_open_failed");
        return false;
    }

    uint8_t buf[GIF_CHUNK_SIZE];
    uint32_t received = 0;
    uint32_t lastAcked = 0;
    unsigned long lastByte = millis();

    // Um ack por leitura da serial (um bloco do host chega em varias leituras
    // de ~64 bytes) enche a USB de mensagens pequenas e a placa trava no meio
    // de envios grandes. Entao confirma so quando ja chegou ACK_STEP bytes
    // desde o ultimo ack, ou quando a serial esvaziou (o bloco do host
    // terminou) — o host espera `received` cumulativo, entao tanto faz.
    auto ack = [&]() {
        Serial.printf("{\"ok\":true,\"received\":%lu}\n", (unsigned long)received);
        lastAcked = received;
    };

    while (received < size) {
        int available = Serial.available();
        if (available > 0) {
            uint32_t remaining = size - received;
            size_t want = (size_t)available;
            if (want > sizeof(buf)) want = sizeof(buf);
            if (want > remaining) want = remaining;
            int n = Serial.readBytes(buf, want);
            if (n > 0) {
                if (f.write(buf, n) != (size_t)n) { // flash cheia/corrompida
                    f.close();
                    LittleFS.remove(tmpPath);
                    Serial.printf("{\"ok\":false,\"error\":\"write_failed\",\"received\":%lu}\n",
                                  (unsigned long)received);
                    return false;
                }
                received += n;
                lastByte = millis();
                if (received == size || received - lastAcked >= ACK_STEP) ack();
            }
            // delay(1) de verdade (nao so yield()): a task do TinyUSB que
            // bombeia bytes novos da USB pro ring buffer do Serial roda com
            // prioridade mais baixa, entao so um yield() cooperativo nao
            // garante que ela rode. Sem isso, um loop que nunca encontra o
            // ramo "sem dados" pode starvar essa task e o dispositivo para
            // de receber bytes novos mesmo com o host ainda mandando.
            delay(1);
        } else if (millis() - lastByte > RECV_TIMEOUT_MS) {
            break;
        } else {
            // Serial vazia: se o host mandou menos que ACK_STEP e parou
            // (bloco menor), confirma o que chegou pra ele continuar.
            if (received > lastAcked && millis() - lastByte >= 3) ack();
            delay(1);
        }
    }
    f.close();

    if (received != size) {
        LittleFS.remove(tmpPath);
        Serial.printf("{\"ok\":false,\"error\":\"timeout\",\"received\":%lu,\"size\":%lu}\n",
                      (unsigned long)received, (unsigned long)size);
        return false;
    }
    return true;
}

void handlePutGif(JsonDocument &doc) {
    uint32_t size = doc["size"] | 0;
    if (size == 0) {
        sendError("invalid_size");
        return;
    }
    if (size > MAX_GIF_SIZE) {
        Serial.printf("{\"ok\":false,\"error\":\"too_large\",\"max\":%lu}\n",
                      (unsigned long)MAX_GIF_SIZE);
        return;
    }
    Serial.println("{\"ok\":true}");
    if (!receiveBytes(GIF_TMP_PATH, size)) return;

    Screensaver::stop();
    if (LittleFS.exists(GIF_PATH)) LittleFS.remove(GIF_PATH);
    LittleFS.rename(GIF_TMP_PATH, GIF_PATH);
    Serial.printf("{\"ok\":true,\"written\":%lu}\n", (unsigned long)size);
}

void handleDeleteGif() {
    Screensaver::stop();
    if (LittleFS.exists(GIF_PATH)) LittleFS.remove(GIF_PATH);
    Serial.println("{\"ok\":true}");
}

void handlePutIcon(JsonDocument &doc) {
    int index = doc["index"] | -1;
    uint32_t size = doc["size"] | 0;
    if (index < 0 || index >= MAX_BUTTONS) {
        sendError("invalid_index");
        return;
    }
    if (size != Icon::BYTES) {
        Serial.printf("{\"ok\":false,\"error\":\"invalid_size\",\"expected\":%u}\n",
                      (unsigned)Icon::BYTES);
        return;
    }
    Serial.println("{\"ok\":true}");
    if (!receiveBytes(ICON_TMP_PATH, size)) return;

    String finalPath = Config::iconPath((uint8_t)index);
    if (LittleFS.exists(finalPath)) LittleFS.remove(finalPath);
    LittleFS.rename(ICON_TMP_PATH, finalPath);
    if (gOnAssetsChanged) gOnAssetsChanged();
    Serial.printf("{\"ok\":true,\"written\":%lu}\n", (unsigned long)size);
}

void handlePutBg(JsonDocument &doc) {
    uint32_t size = doc["size"] | 0;
    if (size != BG_BYTES) {
        Serial.printf("{\"ok\":false,\"error\":\"invalid_size\",\"expected\":%u}\n",
                      (unsigned)BG_BYTES);
        return;
    }

    // O fundo tem ~300KB e a particao e pequena (1.5MB, dividida com o GIF e
    // os icones): sem espaco pro arquivo temporario, apaga o fundo antigo
    // antes de receber o novo.
    auto freeBytes = []() { return LittleFS.totalBytes() - LittleFS.usedBytes(); };
    if (freeBytes() < size && LittleFS.exists(BG_PATH)) LittleFS.remove(BG_PATH);
    if (freeBytes() < size + 8192) {
        Serial.printf("{\"ok\":false,\"error\":\"no_space\",\"free\":%lu}\n",
                      (unsigned long)freeBytes());
        return;
    }

    Serial.println("{\"ok\":true}");
    if (!receiveBytes(BG_TMP_PATH, size)) return;

    if (LittleFS.exists(BG_PATH)) LittleFS.remove(BG_PATH);
    LittleFS.rename(BG_TMP_PATH, BG_PATH);
    if (gOnAssetsChanged) gOnAssetsChanged();
    Serial.printf("{\"ok\":true,\"written\":%lu}\n", (unsigned long)size);
}

void handleDeleteBg() {
    if (LittleFS.exists(BG_PATH)) LittleFS.remove(BG_PATH);
    if (gOnAssetsChanged) gOnAssetsChanged();
    Serial.println("{\"ok\":true}");
}

// Fundo da tela de uso do Claude: arquivo RLE (tamanho variavel, ver Usage.h).
void handlePutUsageBg(JsonDocument &doc) {
    uint32_t size = doc["size"] | 0;
    if (size == 0 || size % 3 != 0 || size > USAGE_BG_MAX_BYTES) {
        Serial.printf("{\"ok\":false,\"error\":\"invalid_size\",\"max\":%lu}\n",
                      (unsigned long)USAGE_BG_MAX_BYTES);
        return;
    }

    // Sem espaco pro arquivo temporario, apaga o fundo antigo antes (vai ser
    // substituido de qualquer jeito).
    auto freeBytes = []() { return LittleFS.totalBytes() - LittleFS.usedBytes(); };
    if (freeBytes() < size + 8192 && LittleFS.exists(USAGE_BG_PATH)) LittleFS.remove(USAGE_BG_PATH);
    if (freeBytes() < size + 8192) {
        Serial.printf("{\"ok\":false,\"error\":\"no_space\",\"free\":%lu}\n", (unsigned long)freeBytes());
        return;
    }

    Serial.println("{\"ok\":true}");
    if (!receiveBytes(USAGE_BG_TMP_PATH, size)) return;

    if (LittleFS.exists(USAGE_BG_PATH)) LittleFS.remove(USAGE_BG_PATH);
    LittleFS.rename(USAGE_BG_TMP_PATH, USAGE_BG_PATH);
    if (gOnAssetsChanged) gOnAssetsChanged();
    Serial.printf("{\"ok\":true,\"written\":%lu}\n", (unsigned long)size);
}

void handleDeleteUsageBg() {
    if (LittleFS.exists(USAGE_BG_PATH)) LittleFS.remove(USAGE_BG_PATH);
    if (gOnAssetsChanged) gOnAssetsChanged();
    Serial.println("{\"ok\":true}");
}

void handleDeleteIcon(JsonDocument &doc) {
    int index = doc["index"] | -1;
    if (index < 0 || index >= MAX_BUTTONS) {
        sendError("invalid_index");
        return;
    }
    String path = Config::iconPath((uint8_t)index);
    if (LittleFS.exists(path)) LittleFS.remove(path);
    if (gOnAssetsChanged) gOnAssetsChanged();
    Serial.println("{\"ok\":true}");
}

// Diagnostico: espaco da particao LittleFS e arquivos gravados.
void handleFsInfo() {
    JsonDocument resp;
    resp["ok"] = true;
    resp["total"] = LittleFS.totalBytes();
    resp["used"] = LittleFS.usedBytes();
    JsonArray files = resp["files"].to<JsonArray>();
    File root = LittleFS.open("/");
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        JsonObject o = files.add<JsonObject>();
        o["n"] = String(f.name());
        o["s"] = (uint32_t)f.size();
    }
    serializeJson(resp, Serial);
    Serial.println();
}

void handleNotify(JsonDocument &doc) {
    const char *app = doc["app"] | "";
    const char *title = doc["title"] | "";
    const char *body = doc["body"] | "";
    Notification::setIcon(doc["icon"] | "");
    if (gOnNotify) gOnNotify(app, title, body);
    Serial.println("{\"ok\":true}");
}

void handleReboot() {
    Serial.println("{\"ok\":true}");
    Serial.flush();
    delay(100);
    ESP.restart();
}

void handleLine(char *line) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
        sendError("invalid_json");
        return;
    }

    const char *cmd = doc["cmd"] | "";
    if (strcmp(cmd, "ping") == 0) {
        handlePing();
    } else if (strcmp(cmd, "get_config") == 0) {
        handleGetConfig();
    } else if (strcmp(cmd, "set_config") == 0) {
        handleSetConfig(line);
    } else if (strcmp(cmd, "put_gif") == 0) {
        handlePutGif(doc);
    } else if (strcmp(cmd, "delete_gif") == 0) {
        handleDeleteGif();
    } else if (strcmp(cmd, "put_icon") == 0) {
        handlePutIcon(doc);
    } else if (strcmp(cmd, "delete_icon") == 0) {
        handleDeleteIcon(doc);
    } else if (strcmp(cmd, "fs_info") == 0) {
        handleFsInfo();
    } else if (strcmp(cmd, "put_bg") == 0) {
        handlePutBg(doc);
    } else if (strcmp(cmd, "delete_bg") == 0) {
        handleDeleteBg();
    } else if (strcmp(cmd, "put_usage_bg") == 0) {
        handlePutUsageBg(doc);
    } else if (strcmp(cmd, "delete_usage_bg") == 0) {
        handleDeleteUsageBg();
    } else if (strcmp(cmd, "status") == 0) {
        Status::update(doc);
        Serial.println("{\"ok\":true}");
    } else if (strcmp(cmd, "usage") == 0) {
        Usage::update(doc);
        Serial.println("{\"ok\":true}");
    } else if (strcmp(cmd, "notify") == 0) {
        handleNotify(doc);
    } else if (strcmp(cmd, "reboot") == 0) {
        handleReboot();
    } else {
        sendError("unknown_cmd");
    }
}

} // namespace

namespace SerialProtocol {

void begin(ApplyConfigFn onConfigApplied, AssetsChangedFn onAssetsChanged, NotifyFn onNotify) {
    gOnConfigApplied = onConfigApplied;
    gOnAssetsChanged = onAssetsChanged;
    gOnNotify = onNotify;
    gLineLen = 0;
}

void poll() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            gLineBuf[gLineLen] = '\0';
            if (gLineLen > 0) handleLine(gLineBuf);
            gLineLen = 0;
        } else if (c != '\r') {
            if (gLineLen < LINE_BUF_SIZE - 1) {
                gLineBuf[gLineLen++] = c;
            } else {
                gLineLen = 0; // linha longa demais, descarta
            }
        }
    }
}

} // namespace SerialProtocol
