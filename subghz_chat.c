#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <furi_hal_subghz.h>
#include <string.h>

#define CHAT_FREQ 433920000 

// Структура данных для радио
typedef struct {
    char last_rx_msg[64];
    bool is_external;
    bool new_msg_received;
} ChatModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    TextInput* text_input;
    char tx_buf[64];
    FuriTimer* rx_timer; // Таймер для проверки приема
} ChatApp;

// Отрисовка
static void render_callback(Canvas* canvas, void* model) {
    ChatModel* m = model;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Sub-GHz Messenger");
    
    canvas_set_font(canvas, FontSecondary);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_str(canvas, 2, 25, m->is_external ? "Ant: EXTERNAL (GPIO)" : "Ant: INTERNAL");
    canvas_draw_line(canvas, 0, 28, 128, 28);
    
    canvas_draw_str(canvas, 2, 42, m->new_msg_received ? "NEW MESSAGE:" : "Last RX:");
    canvas_draw_str(canvas, 2, 53, m->last_rx_msg[0] ? m->last_rx_msg : "Waiting...");
    
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 63, "OK: Write | UP/DN: Ant");
}

// Отправка данных в эфир
static void send_radio_packet(ChatApp* app) {
    size_t len = strlen(app->tx_buf);
    if(len == 0) return;

    // Подготовка радио
    furi_hal_subghz_idle();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok270Async);
    furi_hal_subghz_set_frequency(CHAT_FREQ);

    // Пакет: [Magic 0x43] [Length] [Data...]
    uint8_t packet[66];
    packet[0] = 0x43; // "C" - Chat magic
    packet[1] = (uint8_t)len;
    memcpy(&packet[2], app->tx_buf, len);

    furi_hal_subghz_start_async_tx(NULL, NULL); // Инициализация TX
    furi_hal_subghz_transmit(packet, len + 2);
    furi_delay_ms(50);
    furi_hal_subghz_stop_async_tx();
    
    furi_hal_subghz_rx(); // Возврат в режим приема
}

// Проверка приема (упрощенный поллинг для стабильности)
static void rx_check_timer(void* ctx) {
    ChatApp* app = ctx;
    if(furi_hal_subghz_is_rx_data_complete()) {
        uint8_t buffer[66];
        size_t size = furi_hal_subghz_get_rx_data(buffer, sizeof(buffer));
        
        if(size > 2 && buffer[0] == 0x43) {
            uint8_t len = buffer[1];
            if(len < 64) {
                with_view_model(app->main_view, ChatModel * m, {
                    memcpy(m->last_rx_msg, &buffer[2], len);
                    m->last_rx_msg[len] = '\0';
                    m->new_msg_received = true;
                }, true);
            }
        }
        furi_hal_subghz_rx(); // Рестарт RX
    }
}

static void text_input_done(void* ctx) {
    ChatApp* app = ctx;
    send_radio_packet(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
}

static bool input_callback(InputEvent* event, void* ctx) {
    ChatApp* app = ctx;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            view_dispatcher_switch_to_view(app->view_dispatcher, 1);
            return true;
        } else if(event->key == InputKeyUp || event->key == InputKeyDown) {
            with_view_model(app->main_view, ChatModel * m, {
                m->is_external = !m->is_external;
                furi_hal_subghz_set_path(m->is_external ? FuriHalSubGhzPathIsolate : FuriHalSubGhzPathMain);
            }, true);
            return true;
        }
    }
    return false;
}

static uint32_t exit_callback(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

int32_t subghz_chat_app(void* p) {
    UNUSED(p);
    ChatApp* app = malloc(sizeof(ChatApp));
    memset(app, 0, sizeof(ChatApp));
    
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();

    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLockFree, sizeof(ChatModel));
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, render_callback);
    view_set_input_callback(app->main_view, input_callback);
    view_set_previous_callback(app->main_view, exit_callback);

    app->text_input = text_input_alloc();
    text_input_set_result_callback(app->text_input, text_input_done, app, app->tx_buf, 64, true);
    text_input_set_header_text(app->text_input, "Enter Message (EN):");
    view_set_previous_callback(text_input_get_view(app->text_input), exit_callback);

    view_dispatcher_add_view(app->view_dispatcher, 0, app->main_view);
    view_dispatcher_add_view(app->view_dispatcher, 1, text_input_get_view(app->text_input));
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    // Настройка радио при старте
    furi_hal_subghz_init();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok270Async);
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    furi_hal_subghz_rx();

    // Запуск таймера проверки эфира (каждые 200 мс)
    app->rx_timer = furi_timer_alloc(rx_check_timer, FuriTimerTypePeriodic, app);
    furi_timer_start(app->rx_timer, 200);

    view_dispatcher_run(app->view_dispatcher);

    // Очистка
    furi_timer_stop(app->rx_timer);
    furi_timer_free(app->rx_timer);
    furi_hal_subghz_idle();
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
