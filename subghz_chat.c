#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <subghz/subghz_worker.h>

#define TAG "SubGhzChat"
#define CHAT_FREQ 433920000 // 433.92 MHz

// Кастомный пакет данных
typedef struct {
    uint32_t magic;    // 0xDEADC0DE для фильтрации шума
    uint8_t length;    // Длина текста
    char text[64];     // Само сообщение
    uint8_t crc;       // XOR чексумма
} ChatPacket;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    TextInput* text_input;
    
    char last_rx_msg[65];
    char tx_buf[65];
    bool is_external;
} ChatApp;

// Простейшая проверка целостности
static uint8_t calc_crc(ChatPacket* pkt) {
    uint8_t crc = 0;
    for(size_t i = 0; i < pkt->length; i++) crc ^= pkt->text[i];
    return crc;
}

// Рендеринг основного экрана
static void render_callback(Canvas* canvas, void* ctx) {
    ChatApp* app = ctx;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Messenger");
    
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 25, app->is_external ? "Antenna: EXTERNAL" : "Antenna: INTERNAL");
    
    canvas_draw_line(canvas, 0, 28, 128, 28);
    canvas_draw_str(canvas, 2, 42, "Last RX:");
    canvas_draw_str(canvas, 2, 52, app->last_rx_msg[0] ? app->last_rx_msg : "No messages...");
    
    canvas_draw_str(canvas, 2, 62, "OK: Write | UP/DN: Ant");
}

// Логика отправки
static void send_message(ChatApp* app) {
    ChatPacket pkt = { .magic = 0xDEADC0DE, .length = strlen(app->tx_buf) };
    strncpy(pkt.text, app->tx_buf, 63);
    pkt.crc = calc_crc(&pkt);

    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    // Пресет для макс. дальности
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok270Async); 
    
    furi_hal_subghz_start_packet_tx((uint8_t*)&pkt, sizeof(ChatPacket));
    furi_delay_ms(100); // Даем время на передачу
    furi_hal_subghz_stop_packet_tx();
    furi_hal_subghz_rx(); // Возвращаемся в режим приема
}

// Коллбэк ввода текста
static void text_input_done(void* ctx) {
    ChatApp* app = ctx;
    send_message(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
}

// Обработка кнопок
static bool input_callback(InputEvent* event, void* ctx) {
    ChatApp* app = ctx;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            view_dispatcher_switch_to_view(app->view_dispatcher, 1);
            return true;
        } else if(event->key == InputKeyUp || event->key == InputKeyDown) {
            app->is_external = !app->is_external;
            furi_hal_subghz_set_path(app->is_external ? FuriHalSubGhzPathIsolate : FuriHalSubGhzPathInternal);
            // Принудительно выставляем тип устройства для внешнего модуля
            furi_hal_subghz_set_device_type(app->is_external ? FuriHalSubGhzDeviceTypeExternal : FuriHalSubGhzDeviceTypeInternal);
            return true;
        }
    }
    return false;
}

int32_t subghz_chat_app(void* p) {
    ChatApp* app = malloc(sizeof(ChatApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    
    // Инициализация радио
    furi_hal_subghz_init();
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    app->is_external = false;

    // Главный экран
    app->main_view = view_alloc();
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, render_callback);
    view_set_input_callback(app->main_view, input_callback);

    // Экран ввода
    app->text_input = text_input_alloc();
    text_input_set_result_callback(app->text_input, text_input_done, app, app->tx_buf, 64, true);
    text_input_set_header_text(app->text_input, "Message:");

    view_dispatcher_add_view(app->view_dispatcher, 0, app->main_view);
    view_dispatcher_add_view(app->view_dispatcher, 1, text_input_get_view(app->text_input));
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    // Запуск приема
    furi_hal_subghz_rx();

    // Главный цикл (упрощенный пример)
    view_dispatcher_run(app->view_dispatcher);

    // Очистка
    furi_hal_subghz_sleep();
    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_dispatcher_remove_view(app->view_dispatcher, 1);
    text_input_free(app->text_input);
    view_free(app->main_view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
