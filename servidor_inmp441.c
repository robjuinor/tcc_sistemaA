// Bibliotecas de funcionamento geral:
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "math.h"
#include "freertos/task.h"  // Incluir biblioteca relacionada a tarefas ou interrupcoes

//Bibliotecas de funcionamento especifico:
#include "freertos/queue.h"  // Incluir bibliotecas que lidam com filas ou buffers
#include "driver/i2s_std.h"  // Incluir biblioteca que controla o modo standard da comunicacao i2s

//Bibliotecas relacionadas a comunicaçao Wi-fi, internet e sockets:
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_event.h"
#include "esp_system.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "cred_wifi.h"  // Arquivo contendo credenciais para conexao wi-fi

// Definicoes gerais:
#define LED                             GPIO_NUM_2  // Porta do led integrado do ESP32

//Definicoes para comunicacao I2S:
#define BCLK_IO    GPIO_NUM_5     // Porta do bit clock
#define WS_IO      GPIO_NUM_15     // Porta do word select
#define DIN_IO     GPIO_NUM_4     // Porta da entrada de dados
#define TAM_BUFFER_I2S               16  // Tamanho do buffer para receber dados da comunicacao I2S (deve ser igual ao do cliente)
#define FREQ_AMOST                      44100  // Frequencia de amostragem desejada em Hz (deve ser igual a do cliente)
#define NAMOST                          65536  // Numero de amostras padrao, caso o usuario escolha um numero invalido no cliente websocket
#define RAZAO_BUFFER_AMOST                2  // Razao entre numero de elementos de um buffer e quantas amostras ele representa (um buffer de 8 bits precisa de dois elementos para  cada amostra de 16 bits) (deve ser igual a do cliente)

// Definicoes para comunicacao websocket:
#define CONFIG_BUFF_SIZE                1024  // Tamanho do buffer para receber mensagens via websocket
#define PORT                            3333  // Porta de comunicacao para servidor websocket

// Handles:
static i2s_chan_handle_t                rx_chan;        // Handle para canal de recepcao de informacoes I2S
TaskHandle_t thandle_i2s, thandle_tcp, thandle_blink;  // Handle para as tarefas principais de I2S, Websocket TCP e piscar LED
QueueHandle_t queue;  // Handle para o buffer fifo

// Variaveis globais de amostragem:
int nleituras =  NAMOST * RAZAO_BUFFER_AMOST / TAM_BUFFER_I2S;  // Numero de leituras do canal I2S para obter numero desejado de amostras
int flag_comecar = 0;  // Flag para iniciar leituras do I2S apos configuracoes enviadas pelo cliente

// Tarefa de piscar LED (usada para indicar que o ESP32 esta conectado a internet e esperando cliente):
static void blink_task(void *arg)
{
    while(1)
    {
        gpio_set_level(LED, 1);
        vTaskDelay(250 / portTICK_PERIOD_MS);
        gpio_set_level(LED, 0);
        vTaskDelay(250 / portTICK_PERIOD_MS);
    }
}

// Tratador de evento de wi-fi, usado ao configurar a conexao wi-fi:
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        printf("WiFi connecting WIFI_EVENT_STA_START ... \n");
        break;
    case WIFI_EVENT_STA_CONNECTED:
        printf("WiFi connected WIFI_EVENT_STA_CONNECTED ... \n");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        printf("WiFi lost connection WIFI_EVENT_STA_DISCONNECTED ... \n");
        esp_restart();
        break;
    case IP_EVENT_STA_GOT_IP:
        printf("WiFi got IP ... \n\n");
        vTaskResume(thandle_blink);  // Retoma tarefa de piscar o LED caso haja conexao a internet
        break;
    default:
        break;
    }
}

// Configura conexao wi-fi, as credenciais para SSID e senha(PASS) devem ser digitadas no arquivo my_data.h:
void conexao_wifi()
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_initiation);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    wifi_config_t wifi_configuration = {
        .sta = {
            .ssid = SSID,
            .password = PASS}};
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_connect();
}

// Tarefa que cuida da comunicacao do servidor websocket TCP
static void tarefa_servidor_tcp(void *pvParameters)
{
    // Variaveis de comunicacao websocket
    int sock;  // Variavel que guarda o socket
    static const char *TAG = "TCP_SOCKET";  // Variavel para visualizacao serial 

    int k, k2, k3;  // Inteiros auxiliares
    char strbuff[CONFIG_BUFF_SIZE + 1];  // buffer para receber mensagens via websocket
    int namost;  // Variavel auxiliar que recebe numero de amostras desejado
    int envios = 0; // Numero de amostras enviadas por websocket TCP

    // Buffer de 8 bits para recebimento via I2S:
    int8_t *bufi2s = (int8_t *)calloc(1, TAM_BUFFER_I2S);
    assert(bufi2s);

    //Variaveis padrao do exemplo websocket TCP:
    char addr_str[128];
    int keepAlive = 1;
    int keepIdle = 5;
    int keepInterval = 5;
    int keepCount = 3;
    struct sockaddr_storage dest_addr;

    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(PORT);
    
    // Criar socket:
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0); // 0 para protocolo TCP
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "Socket created");

    bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    // Escutar socket:
    listen(listen_sock, 1);

    while (1)
    {
        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr; // salvar endereco ipv4 ou ipv6
        socklen_t addr_len = sizeof(source_addr);

        // Aceitar socket
        sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0)
        {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Configurar opcao keepalive tcp
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        // Converter endereco ip para string
        if (source_addr.ss_family == PF_INET)
        {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        }

        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        // Recebendo configuracoes do cliente Socket:
        recv(sock, &strbuff, CONFIG_BUFF_SIZE, 0);  // Recebendo mensagem do cliente
        
        // Lendo string de configuracoes (escalonavel para poder receber outras configuracoes):
        // formato das configuracoes: Aconf1Aconf2Aconf3...X 
        // sendo conf1 = configuracao 1, conf2 = configuracao 2....
        k = 0;
        namost = 0;
        while (strbuff[k] != 'X')
        {
            if (strbuff[k] == 'A')
            {
                k++;
                k2 = k;
                while(strbuff[k] != 'X' && strbuff[k] != 'A')
                {
                    k++;
                }
                k2 = k - k2;
                k3 = 0;
                for (k3 = 0; k3 < k2; k3++)
                {
                    namost = namost + (strbuff[k - k2 + k3] - 48) * pow(10, k2 - 1 - k3);
                }
            }
        }

        // Configuracao 1: Numero de amostras desejado:
        if(namost %4 != 0 || namost > 30 * FREQ_AMOST || namost <0)
        {}
        else
        {
            nleituras = namost * RAZAO_BUFFER_AMOST / TAM_BUFFER_I2S;
        }


        flag_comecar = 1;  // Flag para que o modulo I2S espere o cliente estar pronto
        envios = 0;  // Usada para garantir que o numero de buffers enviados via socket sera igual ao numero de leituras I2S, ou seja, nenhum buffer foi perdido na queue FIFO
        while(envios < nleituras)
        {
            if(xQueueReceive(queue, bufi2s, pdMS_TO_TICKS(100)))  // Confere chegou algum buffer na queue FIFO
            {
                send(sock, bufi2s, TAM_BUFFER_I2S, 0);  // Envia o buffer obtido na leitura I2S para o cliente via websocket
                envios++;
            }
        }
        // Fechando o socket:
        shutdown(sock, 0);  
        close(sock);
        vTaskDelay(500 / portTICK_PERIOD_MS);

        // Reiniciando o ESP32:
        esp_restart();
    }
    close(listen_sock);
    vTaskDelete(NULL);
}

static void tarefa_leitura_i2s(void *args)
{
    // Variaveis de amostragem
    int leituras = 0;  // Numero de leituras feitas no canal I2S

    // Alocacao de buffer para leituras I2S:
    int8_t *r_buf = (int8_t *)calloc(1, TAM_BUFFER_I2S);
    assert(r_buf);  // Confere se alocacao foi feita com sucesso
    size_t r_bytes = 0;

    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));  // Habilita canal de recepcao

    vTaskDelay(2000 / portTICK_PERIOD_MS);  // Descartando amostras ruins
    while(1)
    {
        while(flag_comecar == 0)  // Aguardando o cliente estar pronto
        {     
        }
        vTaskSuspend(thandle_blink);  // Para de piscar led
        gpio_set_level(LED, 1);  // Deixa led aceso
        while (leituras < nleituras)
        {    
            if (i2s_channel_read(rx_chan, r_buf, TAM_BUFFER_I2S, &r_bytes, portMAX_DELAY) == ESP_OK)  // Leitura do canal de recepcao I2S
            {
                xQueueSend(queue, (void *)r_buf, pdMS_TO_TICKS(100));  // Buffer com bits lidos entra na fila
                leituras++;
            } else {
                printf("Read Task: i2s read failed\n");
            }
        }
        flag_comecar = 0;
        leituras = 0;
        gpio_set_level(LED, 0);
    }
    free(r_buf);
    vTaskDelete(NULL);
}

static void iniciar_i2s_std_simplex(void)
{
    // Criando canal de recepcao como mestre, ou seja, o microcontrolador vai gerar o bit clock (BCLK) e word select (WS):
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan));

    // Configuracoes para usar canal no modo padrao (standard ou std)
    i2s_std_config_t rx_std_cfg = {
        // Configuracao de clock:
        .clk_cfg = {
            .sample_rate_hz = FREQ_AMOST,  // Frequencia de amostragem em Hz
            .clk_src = I2S_CLK_SRC_APLL,  // Fonte de clock de alta frequencia recomendada para audio. O valor padrao eh I2S_CLK_SRC_PLL_160M
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,  // Multiplo do mclock, nao usado, mas quando nao configurado da erro no programa
        },
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),  // Formato MSB mono com largura de bits de dados de 16 bits
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,    // Sinal de mclock nao usado
            .bclk = BCLK_IO,  // Porta do bit clock
            .ws   = WS_IO,  // Porta do word select
            .dout = I2S_GPIO_UNUSED,  // Saida de dados nao usada
            .din  = DIN_IO,  // Porta do data in ou SD
            .invert_flags = {  // Nao inverte nenhum sinal
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    rx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // Leitura do I2s feita no canal esquerdo
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &rx_std_cfg));  // Inicializa o canal I2S
}

void app_main(void)
{
    // Configuracao piscar LED
    gpio_set_direction(LED, GPIO_MODE_OUTPUT);  // Define porta do led como saida
    xTaskCreate(blink_task, "blink", 4096, NULL, 5 , &thandle_blink);  // Cria tarefa de piscar led com prioridade 5, handle usado ao parar ou retomar tarefa
    vTaskSuspend(thandle_blink);

    // Ciar queue do tipo FIFO(primeiro a entrar, primeiro a sair). Usado para levar o buffer lido no canal I2S até a tarefa de comunicacao via socket
    queue = xQueueCreate(5, TAM_BUFFER_I2S);  // cria queue com 5 elementos com mesmo tamanho do buffer

    iniciar_i2s_std_simplex();  // Inicia canal de recepcao I2S

    conexao_wifi();     // Inicia conecao Wi-fi
    vTaskDelay(500 / portTICK_PERIOD_MS);  // Atraso 0.5s

    // Tarefa de comunicacao websocket:
    xTaskCreate(tarefa_servidor_tcp, "tcp_server", 4096, (void *)AF_INET, 5 , &thandle_tcp);

    // Tarefa de leitura I2S
    xTaskCreate(tarefa_leitura_i2s, "tarefa_leitura_i2s", 4096, NULL, 5, &thandle_i2s);
}
