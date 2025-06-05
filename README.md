# Descrição do Projeto: MQTT de Controle e Monitoramento

## Objetivo Geral

Desenvolver um cliente MQTT embarcado no Raspberry Pi Pico W (RP2040) que conecta-se a uma rede Wi-Fi, publica a temperatura interna e controla remotamente LEDs via tópicos MQTT. Serve para demonstrar comunicação bidirecional entre dispositivo IoT e broker MQTT.

## Descrição Funcional

O sistema realiza as seguintes etapas:

1. **Inicialização**:

   * Configura GPIOs de LEDs (verde, azul, vermelho) e ADC para leitura de temperatura a partir do sensor interno.
   * Inicializa interface Wi-Fi e conecta na rede definida por `WIFI_SSID` e `WIFI_PASSWORD`.
   * Obtém endereço IP via DHCP e resolve o host do broker MQTT (`MQTT_SERVER`) por DNS.
2. **Cliente MQTT**:

   * Monta um identificador único de cliente concatenando `MQTT_DEVICE_NAME` e parte do `unique_id` do Pico.
   * Conecta ao broker MQTT usando credenciais (`MQTT_USERNAME`, `MQTT_PASSWORD`) e configura tópico de last-will (`/online`).
   * Após conexão, inscreve-se nos tópicos:

     * `/lampada/todos`
     * `/lampada/1`, `/lampada/2`, `/lampada/3`, `/lampada/E`
   * Para cada mensagem recebida, a callback descobre qual lâmpada deve ser ligada/desligada e aciona os respectivos LEDs: `green_led`, `blue_led`, `red_led` e o LED de status interno do Wi-Fi (pino CYW43\_WL\_GPIO\_LED\_PIN).
3. **Publicação de Temperatura**:

   * A cada 10 s, a task agendada (`temperature_worker_fn`) lê o sensor interno (ADC canal 4) e converte para graus Celsius (ou Fahrenheit, conforme macro). Se o valor mudou desde a última publicação, publica `"/temperature"` com o valor formatado.
4. **Manutenção de Estado**:

   * Ao conectar, publica “1” no tópico de last-will (`/online`) para indicar que o dispositivo está online.
   * Caso receba mensagens nos tópicos de lâmpadas, aplica a mudança de estado e publica o novo estado em cada tópico associado.
5. **Loop Principal**:

   * Executa `cyw43_arch_poll()` para processar eventos Wi-Fi e MQTT.
   * Aguarda eventos de rede e callbacks até desconexão.

## Uso dos Periféricos da BitDogLab

* **Wi-Fi (CYW43)**: utiliza o modem interno para conectar-se à rede definida, gerenciar DHCP, DNS e comunicação TCP/IP para o broker MQTT.
* **ADC do RP2040**: canal 4 configurado para ler a temperatura interna. `adc_init()` e `adc_set_temp_sensor_enabled(true)` ativam o sensor.
* **GPIOs de LEDs (GPIO 11, 12, 13)**: controlam três LEDs externos — cada tópico `/lampada/1`, `/lampada/2`, `/lampada/3` mapeia um LED correspondente.
* **LED de status Wi-Fi (CYW43\_WL\_GPIO\_LED\_PIN)**: usado para indicar estado do tópico `/lampada/E` (lâmpada extra), via `cyw43_arch_gpio_put()`.
* **Timers internos e callbacks LWIP**: emprega a infraestrutura LWIP para tarefas assíncronas de DNS e manutenção de conexão MQTT.

## Instalação e Execução

1. **Pré-requisitos**

   * Raspberry Pi Pico W conectado a USB.
   * Pico SDK e toolchain ARM configurados.
   * Broker MQTT acessível no endereço `MQTT_SERVER` (ex.: Mosquitto local).
2. **Configuração**

   * No código, altere `WIFI_SSID`, `WIFI_PASSWORD` e `MQTT_SERVER` para sua rede e broker.
   * Ajuste credenciais MQTT (`MQTT_USERNAME`, `MQTT_PASSWORD`) se necessário.
3. **Compilação**

   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```

   Ou importe com a extensão do Pico no VSCode.
   
4. **Deploy**

   * Coloque o Pico em modo BOOTSEL (segure o botão BOOTSEL ao conectar).
   * Copie o `.uf2` gerado para o drive do Pico.
   * Pico reiniciará e exibirá mensagens no terminal serial (115200 bps).
5. **Verificação**

   * Conecte-se ao broker MQTT com qualquer cliente (ex.: `mosquitto_sub`).
   * Observe tópicos publicados: `/temperature`, `/online`, `/lampada/*`.
   * Publique em `/lampada/1`, `/lampada/2`, `/lampada/3` ou `/lampada/E` com payload “Ligado” ou “Desligado” para controlar LEDs.

## Autor

Hugo Martins Santana (TIC370101267)
