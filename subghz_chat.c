#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <furi_hal_subghz.h>
#include <notification/notification_messages.h>

#define CHAT_FREQ 433920000 

// Структура сообщения для очереди
typedef struct {
    char text[64];
} ChatMessage;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    TextInput* text_input;
    FuriThread* worker_thread;
    FuriMessageQueue* tx_queue; // Очередь на отправку
    char tx_buf[64];
    volatile bool is_running;
} ChatApp;

// Тот самый безопасный коллбэк для async_tx
static LevelDuration chat_tx_callback(void* context) {
    UNUSED(context);
    // Для простоты шлем импульс. 
    // В идеале тут будет чтение битов из ChatMessage
    return level_duration_make(true, 500); 
}

// ПОТОК РАДИО (аналог из FlipperShare)
static int32_t chat_worker_thread(void* context) {
    ChatApp* app = context;
    ChatMessage tx_msg;

    while(app->is_running) {
        // Проверяем, есть ли что-то на отправку (таймаут 100мс)
        if(furi_message_queue_get(app->tx_queue, &tx_msg, 100) == FuriStatusOk) {
            // ПЕРЕДАЧА
            furi_hal_subghz_idle();
            furi_hal_subghz_set_frequency(CHAT_FREQ);
            if(furi_hal_subghz_start_async_tx(chat_tx_callback, NULL)) {
                furi_delay_ms(100); // Теперь эта задержка НЕ фризит GUI!
                furi_hal_subghz_stop_async_tx();
            }
            furi_hal_subghz_rx();
        }
        
        // Тут можно добавить логику проверки RX
    }
    return 0;
}

// Изменяем функцию отправки: теперь она просто кладет текст в очередь
static void send_radio_packet(ChatApp* app) {
    ChatMessage msg;
    strncpy(msg.text, app->tx_buf, 63);
    furi_message_queue_put(app->tx_queue, &msg, 0); // Мгновенно уходит в поток
}

// Остальные функции (callback-и событий и ввода) остаются как в v3.8, 
// но теперь они вызывают send_radio_packet без страха зависнуть.

int32_t subghz_chat_app(void* p) {
    ChatApp* app = malloc(sizeof(ChatApp));
    memset(app, 0, sizeof(ChatApp));
    app->is_running = true;

    // Инициализация очередей и потоков
    app->tx_queue = furi_message_queue_alloc(8, sizeof(ChatMessage));
    app->worker_thread = furi_thread_alloc_ex("ChatWorker", 1024, chat_worker_thread, app);
    furi_thread_start(app->worker_thread);

    // Стандартная инициализация GUI...
    // (скопируй её из своего v3.8)

    view_dispatcher_run(app->view_dispatcher);

    // Завершение
    app->is_running = false;
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);
    furi_message_queue_free(app->tx_queue);
    
    // Освобождение GUI ресурсов...
    return 0;
}
