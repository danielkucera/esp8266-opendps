#include "ESP8266WiFi.h"
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>

#define MAX_SRV_CLIENTS 4
#define RXBUFFERSIZE 1024
#define STACK_PROTECTOR  512 // bytes
#define HOSTNAME "esp-openDPS"

#define MAX_FRAME_LENGTH 2048
#define _SOF 0x7e
#define _DLE 0x7d
#define _XOR 0x20
#define _EOF 0x7f

#define DPS_TIMEOUT_SEC 3

#define USB_SERIAL Serial

#ifdef USB_SERIAL 
  #include <SoftwareSerial.h>
  #define SOFT_RX D2
  #define SOFT_TX D1

  SoftwareSerial swSerial(SOFT_RX, SOFT_TX);
  #define DPS_SERIAL swSerial

  char serial_buf[MAX_FRAME_LENGTH];
  int serial_len;
#else
  #define DPS_SERIAL Serial
#endif
 
WiFiServer wifiServer(5005);
WiFiClient serverClients[MAX_SRV_CLIENTS];
ESP8266WebServer server(80);

enum states {
  STATE_IDLE,
  STATE_DPS_WAIT_SOF,
  STATE_DPS_WAIT_EOF,
};

int unescape_frame(char* buf, int size){
  int pos = 0;
  for (int i=0; i<size; i++){
    if (buf[i] != _DLE){
      buf[pos++] = buf[i];
    } else {
      i++;
      buf[pos++] = buf[i] ^ _XOR;
    }
  }
  return pos;
}

void send_dps(char* buf, int size, Stream* stream);

int state;
char client_buf[MAX_FRAME_LENGTH];
int client_len;
char dps_buf[MAX_FRAME_LENGTH];
int dps_len;
unsigned long dps_timeout;
Stream* response_stream;

char* get_next_string(char* buf){
  int i = 0;
  while ((buf[i] != '\0')&&(buf[i] != _EOF)){
    i++;
  }
  if (buf[i] == _EOF){
    return NULL;
  } else {
    return &buf[i];
  }
}

void dps_query() {
  char cmd_query[] = { 0x7e, 0x04, 0x40, 0x84, 0x7f};
  char output[2048];

  send_dps(cmd_query, sizeof(cmd_query), NULL);

  while (state != STATE_IDLE){
    loop();
  }

  dps_len = unescape_frame(dps_buf, dps_len);

  #define U16(i) 256*dps_buf[i] + dps_buf[i+1]

  int v_in = U16(3);
  int v_out = U16(5);
  int i_out = U16(7);
  int en = dps_buf[9];
  int temp1 = U16(10);
  int temp2 = U16(12);
  int temp_shutdown = dps_buf[14];

  char *strs[32];
  strs[0] = &dps_buf[15]; //cur_func
  int str_cnt=1;

  char params[1024];
  int params_pos=0;

  //strs[str_cnt] = get_next_string(strs[str_cnt-1]);

  while(strs[str_cnt] = get_next_string(strs[str_cnt-1])){
    str_cnt++;
  }
  str_cnt--;
  
/*
  for (int i=1; i<str_cnt; i+=2){
    params_pos += sprintf(&params[params_pos], "\t\t\"%s\":\"%s\",\n", strs[i], strs[i+1]);
  }
*/
  sprintf(output, "{\n\t\"v_in\":%d,\n\t\"v_out\":%d,\n\t\"i_out\":%d,\n\t\"output_enabled\":%d,\n\t\"temp_shutdown\":%d,\n\t\"cur_func\":%s,\n\t\"params\":%s\n}", 
    v_in, v_out, i_out, en, temp_shutdown, strs[0], strs[1]);
  //server.send_P(200, "text/html", dps_buf, dps_len);
  server.send(200, "text/html", output);
}

void setup() {
  WiFiManager wifiManager;

  //wifiManager.resetSettings();

  wifiManager.setConfigPortalTimeout(120);
  wifiManager.autoConnect(HOSTNAME);

  DPS_SERIAL.begin(9600);

#ifdef USB_SERIAL
  USB_SERIAL.begin(9600);
#endif
 
  wifiServer.begin();

  ArduinoOTA.begin();

  //MDNS.begin(HOSTNAME);  // this doesn't work, wifiManager starts mDNS
  MDNS.setHostname(HOSTNAME);
  MDNS.addService("opendps", "tcp", 5005);
  
  server.begin();
  server.on("/query", dps_query);

  ESP.wdtDisable();

  state = STATE_IDLE;
}

int find_chr(char* buf, char ch, int size){
  for (int i=0; i<size; i++){
    if (buf[i] == ch){
      return i;
    }
  }
  return -1;
}

void send_dps(char* buf, int size, Stream* stream){
  response_stream = stream;
  DPS_SERIAL.write(buf, size);
  dps_len = 0;
  state = STATE_DPS_WAIT_SOF;
  dps_timeout = millis() + 1000 * DPS_TIMEOUT_SEC;
}

void loop() {
  ArduinoOTA.handle();

  ESP.wdtFeed();

  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    ESP.reset();
  }

  //check if there are any new clients
  if (wifiServer.hasClient()) {
    //find free/disconnected spot
    int i;
    for (i = 0; i < MAX_SRV_CLIENTS; i++)
      if (!serverClients[i]) { // equivalent to !serverClients[i].connected()
        serverClients[i] = wifiServer.available();
        break;
      }

    //no free/disconnected spot so reject
    if (i == MAX_SRV_CLIENTS) {
      wifiServer.available().println("busy");
      // hints: server.available() is a WiFiClient with short-term scope
      // when out of scope, a WiFiClient will
      // - flush() - all data will be sent
      // - stop() - automatically too
    }
  }

  switch(state){
    case STATE_IDLE:
    {
#ifdef USB_SERIAL
      //check for data from USB serial
      if (USB_SERIAL.available()){
        if (serial_len == MAX_FRAME_LENGTH){
          serial_len = 0;
        }
        serial_buf[serial_len++] = USB_SERIAL.read();
        if (serial_buf[0] != _SOF){
          serial_len = 0;
        }
        if (serial_buf[serial_len-1] == _EOF){
          send_dps(serial_buf, serial_len, &USB_SERIAL);
          serial_len = 0;
        }
      }
#endif
      //check TCP clients for data
      for (int i = 0; i < MAX_SRV_CLIENTS; i++){
        if (serverClients[i].available()){
          //peek into client buffer
          client_len = serverClients[i].peekBytes(client_buf, serverClients[i].available());
          if (client_len < 2){
            continue;
          } else {
            int sof_pos = find_chr(client_buf, _SOF, client_len);
            if (sof_pos == -1){
              // garbage found
              serverClients[i].readBytes(client_buf, client_len);
              continue;
            }
            if (sof_pos > 0){
              // read garbage on begining and come again
              serverClients[i].readBytes(client_buf, sof_pos);
              continue;
            }
            int eof_pos = find_chr(client_buf, _EOF, client_len);
            if (eof_pos == -1)
              continue;
            client_len = serverClients[i].readBytesUntil(_EOF, client_buf, MAX_FRAME_LENGTH);
            // https://github.com/esp8266/Arduino/issues/6546
            client_buf[client_len++] = _EOF;
            if (client_buf[client_len-1] != _EOF){
              //something strange happened
              serverClients[i].write("uhm");
              continue;
            }
            // write frame to DPS
            send_dps(client_buf, client_len, &serverClients[i]);
            break;
          }
        }
      }
    }
    break;
    case STATE_DPS_WAIT_SOF:
    {
      if (millis() > dps_timeout){
        state = STATE_IDLE;
        response_stream->write("TIMEOUT");
        break;
      }
      if (!DPS_SERIAL.available())
        break;
      dps_buf[0] = DPS_SERIAL.read();
      if (dps_buf[0] != _SOF){
        break;
      }
      dps_len = 1;
      state = STATE_DPS_WAIT_EOF;
    }
    break;
    case STATE_DPS_WAIT_EOF:
    {
      if (millis() > dps_timeout){
        state = STATE_IDLE;
        response_stream->write("TIMEOUT");
        break;
      }
      if (dps_len == MAX_FRAME_LENGTH){
        //oops EOF not found and max length reached
        response_stream->write("FRAME TOO LONG");
        state = STATE_IDLE;
        break;
      }
      if (!DPS_SERIAL.available())
        break;
      dps_buf[dps_len++] = DPS_SERIAL.read();
      if (dps_buf[dps_len-1] != _EOF){
        break;
      } else {
        //EOF was found
        if (response_stream != NULL)
          response_stream->write(dps_buf, dps_len);
        state = STATE_IDLE;
        break;
      }
    }
    break;
  }
}
