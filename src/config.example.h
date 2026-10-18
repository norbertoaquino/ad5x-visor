#pragma once

// Copie este arquivo para config.h e preencha.
// config.h fica fora do git.

#define WIFI_SSID ""
#define WIFI_PASS ""

// Opcional. Apague a linha para o rádio escolher o canal.
// #define WIFI_CHANNEL 6

// 1 usa DHCP. 0 usa o endereço fixo abaixo.
#define WIFI_DHCP 1

#define WIFI_IP ""
#define WIFI_GW ""
#define WIFI_MASK "255.255.255.0"
#define WIFI_DNS ""

#define PRINTER_IP ""

// Número de série e check code da AD5X, os mesmos do aplicativo.
#define PRINTER_SERIAL ""
#define PRINTER_CHECK ""

// Idioma do log: en pt es ru zh ja tr
#define APP_LANG "en"

// false omite o log. true mostra. O botão BOOT inverte enquanto o firmware roda.
#define DEBUG_LOG false
