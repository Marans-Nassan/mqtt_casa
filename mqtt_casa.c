#include "pico/stdlib.h"           
#include "pico/cyw43_arch.h"        
#include "pico/unique_id.h"
#include "hardware/gpio.h"          
#include "hardware/irq.h"           
#include "hardware/adc.h"           
#include "lwip/apps/mqtt.h"         
#include "lwip/apps/mqtt_priv.h"    
#include "lwip/dns.h"               

#define WIFI_SSID "."                  // Substitua pelo nome da sua rede Wi-Fi
#define WIFI_PASSWORD "."            // Substitua pela senha da sua rede Wi-Fi
#define MQTT_SERVER "."          // Substitua pelo endereço do host - broket MQTT: Ex: 192.168.1.107
#define MQTT_USERNAME "."                // Substitua pelo nome da host MQTT - Username
#define MQTT_PASSWORD "."                // Substitua pelo Password da host MQTT - credencial de acesso - caso exista
#define green_led 11
#define blue_led 12
#define red_led 13

typedef struct estado{
    volatile bool l_1;
    volatile bool l_2;
    volatile bool l_3;
    volatile bool l_e;
} estado;
estado L = {false, false, false, false};

#ifndef TEMPERATURE_UNITS
#define TEMPERATURE_UNITS 'C' 
#endif

#ifndef MQTT_SERVER
#error Need to define MQTT_SERVER
#endif

#ifdef MQTT_CERT_INC
#include MQTT_CERT_INC
#endif

#ifndef MQTT_TOPIC_LEN
#define MQTT_TOPIC_LEN 100
#endif

//Dados do cliente MQTT
typedef struct {
    mqtt_client_t* mqtt_client_inst;
    struct mqtt_connect_client_info_t mqtt_client_info;
    char data[MQTT_OUTPUT_RINGBUF_SIZE];
    char topic[MQTT_TOPIC_LEN];
    uint32_t len;
    ip_addr_t mqtt_server_address;
    bool connect_done;
    int subscribe_count;
    bool stop_client;
} MQTT_CLIENT_DATA_T;


#ifndef DEBUG_printf
#ifndef NDEBUG
#define DEBUG_printf printf
#else
#define DEBUG_printf(...)
#endif
#endif

#ifndef INFO_printf
#define INFO_printf printf
#endif

#ifndef ERROR_printf
#define ERROR_printf printf
#endif

#define TEMP_WORKER_TIME_S 10

#define MQTT_KEEP_ALIVE_S 600

#define MQTT_SUBSCRIBE_QOS 1
#define MQTT_PUBLISH_QOS 1
#define MQTT_PUBLISH_RETAIN 0

#define MQTT_WILL_TOPIC "/online"
#define MQTT_WILL_MSG "0"
#define MQTT_WILL_QOS 1

#ifndef MQTT_DEVICE_NAME
#define MQTT_DEVICE_NAME "pico"
#endif

// Definir como 1 para adicionar o nome do cliente aos tópicos, para suportar vários dispositivos que utilizam o mesmo servidor
#ifndef MQTT_UNIQUE_TOPIC
#define MQTT_UNIQUE_TOPIC 0
#endif

void init_led(void); // Inicializa os GPIOs 11 - 13 como saídas e define nível lógico baixo
static float read_onboard_temperature(const char unit); // Lê temperatura interna do RP2040 e converte para unidade especificada (C/F)
static void pub_request_cb(__unused void *arg, err_t err); // Callback para tratamento de erros após publicação MQTT
static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name); // Gera nome completo do tópico MQTT (com ID do dispositivo se habilitado)
static void set_lampadas(bool on_1, bool on_2, bool on_3, bool on_4); // Controla estados das lâmpadas (LEDs físico e Wi-Fi) e atualiza struct L
static void publish_estado_lampadas(MQTT_CLIENT_DATA_T *state); // Publica estados atuais das lâmpadas via MQTT
static void publish_temperature(MQTT_CLIENT_DATA_T *state); // Publica temperatura atual via MQTT se houve mudança desde última leitura
static void sub_request_cb(void *arg, err_t err); // Callback para inscrição em tópicos (incrementa contador de subscriptions)
static void unsub_request_cb(void *arg, err_t err); // Callback para cancelamento de inscrição (decrementa contador de subscriptions)
static void sub_unsub_topics(MQTT_CLIENT_DATA_T* state, bool sub); // Inscreve/desinscreve nos tópicos de controle das lâmpadas
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags); // Processa mensagens recebidas (controle das lâmpadas via comandos MQTT)
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len); // Callback chamado quando nova publicação é recebida em tópico inscrito
static void temperature_worker_fn(async_context_t *context, async_at_time_worker_t *worker); // Função agendada periodicamente para publicar temperatura
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status); // Callback de eventos de conexão MQTT (gerencia subscriptions pós-conexão)
static void start_client(MQTT_CLIENT_DATA_T *state); // Inicia conexão com broker MQTT usando configurações do estado
static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg); // Callback para resolução DNS (inicia cliente após obter IP do broker)
static async_at_time_worker_t temperature_worker = { .do_work = temperature_worker_fn };

int main(void) {

    stdio_init_all();
    init_led();
    INFO_printf("mqtt client starting\n");

    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);

    static MQTT_CLIENT_DATA_T state;

    if (cyw43_arch_init()) {
        panic("Failed to inizialize CYW43");
    }

    char unique_id_buf[5];
    pico_get_unique_board_id_string(unique_id_buf, sizeof(unique_id_buf));
    for(int i=0; i < sizeof(unique_id_buf) - 1; i++) {
        unique_id_buf[i] = tolower(unique_id_buf[i]);
    }

    // Gera nome único, Ex: pico1234
    char client_id_buf[sizeof(MQTT_DEVICE_NAME) + sizeof(unique_id_buf) - 1];
    memcpy(&client_id_buf[0], MQTT_DEVICE_NAME, sizeof(MQTT_DEVICE_NAME) - 1);
    memcpy(&client_id_buf[sizeof(MQTT_DEVICE_NAME) - 1], unique_id_buf, sizeof(unique_id_buf) - 1);
    client_id_buf[sizeof(client_id_buf) - 1] = 0;
    INFO_printf("Device name %s\n", client_id_buf);

    state.mqtt_client_info.client_id = client_id_buf;
    state.mqtt_client_info.keep_alive = MQTT_KEEP_ALIVE_S;
#if defined(MQTT_USERNAME) && defined(MQTT_PASSWORD)
    state.mqtt_client_info.client_user = MQTT_USERNAME;
    state.mqtt_client_info.client_pass = MQTT_PASSWORD;
#else
    state.mqtt_client_info.client_user = NULL;
    state.mqtt_client_info.client_pass = NULL;
#endif
    static char will_topic[MQTT_TOPIC_LEN];
    strncpy(will_topic, full_topic(&state, MQTT_WILL_TOPIC), sizeof(will_topic));
    state.mqtt_client_info.will_topic = will_topic;
    state.mqtt_client_info.will_msg = MQTT_WILL_MSG;
    state.mqtt_client_info.will_qos = MQTT_WILL_QOS;
    state.mqtt_client_info.will_retain = true;


    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        panic("Failed to connect");
    }
    INFO_printf("\nConnected to Wifi\n");

    cyw43_arch_lwip_begin();
    int err = dns_gethostbyname(MQTT_SERVER, &state.mqtt_server_address, dns_found, &state);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        start_client(&state);
    } else if (err != ERR_INPROGRESS) { 
        panic("dns request failed");
    }

    while (!state.connect_done || mqtt_client_is_connected(state.mqtt_client_inst)) {
        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(10000));
    }

    INFO_printf("mqtt client exiting\n");
    return 0;
}

void init_led(void){
    for(uint8_t i = 11; i < 14; i++){
        gpio_init(i);
        gpio_set_dir(i, GPIO_OUT);
        gpio_put(i, 0);
    }
}

static float read_onboard_temperature(const char unit) {

    const float conversionFactor = 3.3f / (1 << 12);

    float adc = (float)adc_read() * conversionFactor;
    float tempC = 27.0f - (adc - 0.706f) / 0.001721f;

    if (unit == 'C' || unit != 'F') {
        return tempC;
    } else if (unit == 'F') {
        return tempC * 9 / 5 + 32;
    }

    return -1.0f;
}

static void pub_request_cb(__unused void *arg, err_t err) {
    if (err != 0) {
        ERROR_printf("pub_request_cb failed %d", err);
    }
}

static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name) {
#if MQTT_UNIQUE_TOPIC
    static char full_topic[MQTT_TOPIC_LEN];
    snprintf(full_topic, sizeof(full_topic), "/%s%s", state->mqtt_client_info.client_id, name);
    return full_topic;
#else
    return name;
#endif
}

static void set_lampadas(bool on_1, bool on_2, bool on_3, bool E) {
    L.l_1 = on_1;
    L.l_2 = on_2;
    L.l_3 = on_3;
    L.l_e = E;

    gpio_put(green_led, on_1);
    gpio_put(blue_led, on_2);
    gpio_put(red_led, on_3);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, E);
}
static void publish_estado_lampadas(MQTT_CLIENT_DATA_T *state) {
    const char* state_1 = L.l_1 ? "Ligado" : "Desligado";
    const char* state_2 = L.l_2 ? "Ligado" : "Desligado";
    const char* state_3 = L.l_3 ? "Ligado" : "Desligado";
    const char* state_4 = L.l_e ? "Ligado" : "Desligado";

    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/lampada/1"), state_1, strlen(state_1), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/lampada/2"), state_2, strlen(state_2), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/lampada/3"), state_3, strlen(state_3), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    mqtt_publish(state->mqtt_client_inst, full_topic(state, "/lampada/E"), state_4, strlen(state_4), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
}

static void publish_temperature(MQTT_CLIENT_DATA_T *state) {
    static float old_temperature;
    const char *temperature_key = full_topic(state, "/temperature");
    float temperature = read_onboard_temperature(TEMPERATURE_UNITS);
    if (temperature != old_temperature) {
        old_temperature = temperature;
        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%.2f", temperature);
        INFO_printf("Publishing %s to %s\n", temp_str, temperature_key);
        mqtt_publish(state->mqtt_client_inst, temperature_key, temp_str, strlen(temp_str), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, state);
    }
}

static void sub_request_cb(void *arg, err_t err) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
    if (err != 0) {
        panic("subscribe request failed %d", err);
    }
    state->subscribe_count++;
}

static void unsub_request_cb(void *arg, err_t err) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
    if (err != 0) {
        panic("unsubscribe request failed %d", err);
    }
    state->subscribe_count--;
    assert(state->subscribe_count >= 0);

    if (state->subscribe_count <= 0 && state->stop_client) {
        mqtt_disconnect(state->mqtt_client_inst);
    }
}

static void sub_unsub_topics(MQTT_CLIENT_DATA_T* state, bool sub) {
    mqtt_request_cb_t cb = sub ? sub_request_cb : unsub_request_cb;
    
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/lampada/todos"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/lampada/1"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/lampada/2"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/lampada/3"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/lampada/E"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
}

static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
#if MQTT_UNIQUE_TOPIC
    const char *basic_topic = state->topic + strlen(state->mqtt_client_info.client_id) + 1;
#else
    const char *basic_topic = state->topic;
#endif
    
    strncpy(state->data, (const char *)data, len);
    state->len = len;
    state->data[len] = '\0';

    DEBUG_printf("Topic: %s, Message: %s\n", state->topic, state->data);
    
    if (strcmp(basic_topic, "/lampada/todos") == 0) {
        bool on = (strcasecmp(state->data, "Ligado") == 0 || strcmp(state->data, "1") == 0);
        set_lampadas(on, on, on, on);
    }
    else if (strncmp(basic_topic, "/lampada/", 9) == 0) {
        const char *led_id = basic_topic + 9;
        bool on = (strcasecmp(state->data, "Ligado") == 0 || strcmp(state->data, "1") == 0);
        if (strcmp(led_id, "1") == 0) set_lampadas(on, L.l_2, L.l_3, L.l_e);
        else if (strcmp(led_id, "2") == 0) set_lampadas(L.l_1, on, L.l_3, L.l_e);
        else if (strcmp(led_id, "3") == 0) set_lampadas(L.l_1, L.l_2, on, L.l_e);
        else if (strcmp(led_id, "E") == 0) set_lampadas(L.l_1, L.l_2, L.l_3, on);
    }
}

static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
    strncpy(state->topic, topic, sizeof(state->topic));
}

static void temperature_worker_fn(async_context_t *context, async_at_time_worker_t *worker) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)worker->user_data;
    publish_temperature(state);
    async_context_add_at_time_worker_in_ms(context, worker, TEMP_WORKER_TIME_S * 1000);
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
    if (status == MQTT_CONNECT_ACCEPTED) {
        state->connect_done = true;
        sub_unsub_topics(state, true); 

        if (state->mqtt_client_info.will_topic) {
            mqtt_publish(state->mqtt_client_inst, state->mqtt_client_info.will_topic, "1", 1, MQTT_WILL_QOS, true, pub_request_cb, state);
        }

        temperature_worker.user_data = state;
        async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &temperature_worker, 0);
    } else if (status == MQTT_CONNECT_DISCONNECTED) {
        if (!state->connect_done) {
            panic("Failed to connect to mqtt server");
        }
    }
    else {
        panic("Unexpected status");
    }
}

static void start_client(MQTT_CLIENT_DATA_T *state) {
#if LWIP_ALTCP && LWIP_ALTCP_TLS
    const int port = MQTT_TLS_PORT;
    INFO_printf("Using TLS\n");
#else
    const int port = MQTT_PORT;
    INFO_printf("Warning: Not using TLS\n");
#endif

    state->mqtt_client_inst = mqtt_client_new();
    if (!state->mqtt_client_inst) {
        panic("MQTT client instance creation error");
    }
    INFO_printf("IP address of this device %s\n", ipaddr_ntoa(&(netif_list->ip_addr)));
    INFO_printf("Connecting to mqtt server at %s\n", ipaddr_ntoa(&state->mqtt_server_address));

    cyw43_arch_lwip_begin();
    if (mqtt_client_connect(state->mqtt_client_inst, &state->mqtt_server_address, port, mqtt_connection_cb, state, &state->mqtt_client_info) != ERR_OK) {
        panic("MQTT broker connection error");
    }
    mqtt_set_inpub_callback(state->mqtt_client_inst, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, state);
    cyw43_arch_lwip_end();
}

static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T*)arg;
    if (ipaddr) {
        state->mqtt_server_address = *ipaddr;
        start_client(state);
    } else {
        panic("dns request failed");
    }
}