#include <lib/subghz/subghz_worker.h> // Добавь это в includes

typedef struct {
    // ... твои поля ...
    SubGhzWorker* worker; // Добавляем воркер в ChatApp
} ChatApp;

// 1. Исправленная функция отправки (без Kernel Panic)
static void send_radio_packet(ChatApp* app) {
    // Проверка региональных ограничений Unleashed
    if(!furi_hal_subghz_is_tx_allowed(CHAT_FREQ)) return;

    // Останавливаем воркер, чтобы освободить чип CC1101
    if(subghz_worker_is_running(app->worker)) subghz_worker_stop(app->worker);

    furi_hal_subghz_idle();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    furi_hal_subghz_set_frequency(CHAT_FREQ);

    // Безопасный старт передачи несущей
    furi_hal_subghz_tx_start();
    furi_delay_ms(100); // Длительность посылки
    furi_hal_subghz_tx_stop();

    furi_hal_subghz_idle();
    // Возвращаемся в режим прослушивания
    subghz_worker_start(app->worker);
}

// 2. Коллбэк для приема данных
static void chat_rx_callback(void* context) {
    ChatApp* app = context;
    with_view_model(app->main_view, ChatModel* m, {
        snprintf(m->last_rx_msg, 64, "Signal Detected!");
    }, true);
}

// 3. В основной функции subghz_chat_app:
int32_t subghz_chat_app(void* p) {
    // ... (аллокация app) ...
    app->worker = subghz_worker_alloc();
    subghz_worker_set_overrun_callback(app->worker, chat_rx_callback, app);

    // Запуск радио
    furi_hal_subghz_idle();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    furi_hal_subghz_set_frequency(CHAT_FREQ);
    subghz_worker_start(app->worker);

    // ... (запуск dispatcher) ...

    // Очистка при выходе
    subghz_worker_stop(app->worker);
    subghz_worker_free(app->worker);
    // ...
}
