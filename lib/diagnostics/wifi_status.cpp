#include "wifi_status.h"

const char *wifi_status_text(int status)
{
    switch (status)
    {
    case SIF_WIFI_IDLE:
        return "radio ocioso, sem tentativa em curso";
    case SIF_WIFI_NO_SSID_AVAIL:
        // The commonest real cause on this bench: the network exists, but on
        // 5 GHz, which no ESP32 can see. Saying so here saves the hour spent
        // suspecting the password.
        return "rede nao encontrada (confira o SSID e se ela esta em 2,4 GHz)";
    case SIF_WIFI_SCAN_COMPLETED:
        return "varredura concluida, ainda sem associacao";
    case SIF_WIFI_CONNECTED:
        return "conectado";
    case SIF_WIFI_CONNECT_FAILED:
        return "associacao recusada (senha errada, ou o AP recusou o dispositivo)";
    case SIF_WIFI_CONNECTION_LOST:
        return "conexao caiu durante a tentativa (sinal fraco?)";
    case SIF_WIFI_DISCONNECTED:
        return "desconectado: o AP respondeu e derrubou, ou nao respondeu a tempo";
    default:
        return "estado desconhecido do radio";
    }
}
