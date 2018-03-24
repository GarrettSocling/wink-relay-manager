#include "wink_relay.h"

#include "MQTTAsync.h"
#include "ini.h"

#include <map>
#include <functional>

// prefix/buttons/index/action/clicks
#define MQTT_BUTTON_TOPIC_FORMAT "%s/buttons/%d/%s/%d"
#define MQTT_BUTTON_CLICK_ACTION "click"
#define MQTT_BUTTON_HELD_ACTION "held"
#define MQTT_BUTTON_RELEASED_ACTION "released"

#define MQTT_RELAY_STATE_TOPIC_FORMAT "%s/relays/%d/state"
#define MQTT_TEMPERATURE_TOPIC_FORMAT "%s/sensors/temperature"
#define MQTT_HUMIDITY_TOPIC_FORMAT "%s/sensors/humidity"

void _onConnectFailure(void* context, MQTTAsync_failureData* response);
void _onConnected(void* context, char* cause);
int _messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message);
int _configHandler(void* user, const char* section, const char* name, const char* value);

enum RelayFlags {
  RELAY_FLAG_NONE = 0,
  RELAY_FLAG_TOGGLE = 1,
  RELAY_FLAG_SEND_CLICK = 1 << 1,
  RELAY_FLAG_SEND_HELD = 1 << 2,
  RELAY_FLAG_SEND_RELEASE = 1 << 3,
};

struct Config {
  std::string mqttClientId = "Relay";
  std::string mqttUsername;
  std::string mqttPassword;
  std::string mqttAddress;
  std::string mqttTopicPrefix = "Relay";
  bool hideStatusBar = true;
  short relayFlags[2] = { RELAY_FLAG_SEND_CLICK | RELAY_FLAG_SEND_HELD, RELAY_FLAG_SEND_CLICK | RELAY_FLAG_SEND_HELD };
};

class WinkRelayManager : public RelayCallbacks {
private:
  using MessageFunction = std::function<void(MQTTAsync_message* msg)>;
  WinkRelay m_relay;
  Config m_config;
  MQTTAsync m_mqttClient;
  std::map<std::string, MessageFunction> m_messageCallbacks;

public:
  void buttonClicked(int button, int count) {
    //printf("button %d clicked. %d clicks\n", button, count);
    if ((m_config.relayFlags[button] & RELAY_FLAG_TOGGLE) && count == 1) {
      m_relay.toggleRelay(button);
    }
    if (m_config.relayFlags[button] & RELAY_FLAG_SEND_CLICK) {
      char topic[256] = {0};
      sprintf(topic, MQTT_BUTTON_TOPIC_FORMAT, m_config.mqttTopicPrefix.c_str(), button, MQTT_BUTTON_CLICK_ACTION, count);
      sendPayload(topic, "ON");
    }
  }
  void buttonHeld(int button, int count) {
    //printf("button %d held. %d clicks\n", button, count);
    if (m_config.relayFlags[button] & RELAY_FLAG_SEND_HELD) {
      char topic[256] = {0};
      sprintf(topic, MQTT_BUTTON_TOPIC_FORMAT, m_config.mqttTopicPrefix.c_str(), button, MQTT_BUTTON_HELD_ACTION, count);
      sendPayload(topic, "ON");
    }
  }
  void buttonReleased(int button, int count) {
    //printf("button %d released. %d clicks\n", button, count);
    if (m_config.relayFlags[button] & RELAY_FLAG_SEND_RELEASE) {
      char topic[256] = {0};
      sprintf(topic, MQTT_BUTTON_TOPIC_FORMAT, m_config.mqttTopicPrefix.c_str(), button, MQTT_BUTTON_RELEASED_ACTION, count);
      sendPayload(topic, "ON");
    }
  }

  void relayStateChanged(int relay, bool state) {
    char topic[256] = {0};
    sprintf(topic, MQTT_RELAY_STATE_TOPIC_FORMAT, m_config.mqttTopicPrefix.c_str(), relay);
    sendPayload(topic, state ? "ON" : "OFF");
  }

  void temperatureChanged(float tempC) {
    char topic[256] = {0};
    sprintf(topic, MQTT_TEMPERATURE_TOPIC_FORMAT, m_config.mqttTopicPrefix.c_str());
    char payload[10] = {0};
    sprintf(payload, "%f", tempC);
    sendPayload(topic, payload);
  }

  void humidityChanged(float humidity) {
    char topic[256] = {0};
    sprintf(topic, MQTT_HUMIDITY_TOPIC_FORMAT, m_config.mqttTopicPrefix.c_str());
    char payload[10] = {0};
    sprintf(payload, "%f", humidity);
    sendPayload(topic, payload);
  }

  void proximityTriggered(int p) {
    //printf("Proximity triggered %d\n", p);
  }

  void onConnected(char* cause) {
    //printf("Successful connection\n");
    int topicCount = m_messageCallbacks.size();
    char* topics[topicCount];
    int qos[topicCount];
    int i=0;

    for (auto it = m_messageCallbacks.begin(); it != m_messageCallbacks.end(); ++it) {
      topics[i] = (char*)(it->first.c_str());
      qos[i++] = 0;
    }
    MQTTAsync_subscribeMany(m_mqttClient, topicCount, topics, qos, nullptr);
    m_relay.resetState(); // trigger fresh state events on next loop
  }

  void onConnectFailure(MQTTAsync_failureData* response) {
    //printf("Connect failed, rc %d\n", response ? response->code : 0);
  }

  void messageArrived(char* topicName, int topicLen, MQTTAsync_message* message) {
    //printf("Received message on topic [%.*s] : %.*s\n", topicLen, topicName, message->payloadlen, message->payload); 
    auto it = m_messageCallbacks.find(topicName);
    if (it != m_messageCallbacks.end()) {
      it->second(message);
    }
  }
  
  void sendPayload(const char* topic, const char* payload) {
    // check if connected?
    int rc;
    if ((rc = MQTTAsync_send(m_mqttClient, topic, strlen(payload), (void*)payload, 0, 0, NULL)) != MQTTASYNC_SUCCESS)
    {
      printf("Failed to send payload, return code %d\n", rc);
    }
  }

  bool processStatePayload(const char* payload, int len, bool& state) {
    if (strncmp(payload, "1", len) == 0 || strncasecmp(payload, "ON", len) == 0) {
      state = true;
      return true;
    } else if (strncmp(payload, "0", len) == 0 || strncasecmp(payload, "OFF", len) == 0) {
      state = false;
      return true;
    }
    return false;
  }

  int handleConfigValue(const char* section, const char* name, const char* value) {
    if (strcmp(name, "mqtt_username") == 0) {
      m_config.mqttUsername = value;
    } else if (strcmp(name, "mqtt_password") == 0) {
      m_config.mqttPassword = value;
    } else if (strcmp(name, "mqtt_clientid") == 0) {
      m_config.mqttClientId = value;
    } else if (strcmp(name, "mqtt_topic_prefix") == 0) {
      m_config.mqttTopicPrefix = value;
    } else if (strcmp(name, "mqtt_address") == 0) {
      m_config.mqttAddress = value;
    } else if (strcmp(name, "screen_timeout") == 0) {
      int timeout = atoi(value);
      if (timeout > 0) {
        m_relay.setScreenTimeout(timeout);
      }
    } else if (strcmp(name, "proximity_threshold") == 0) {
      int t = atoi(value);
      if (t > 0) {
        m_relay.setProximityThreshold(t);
      }
    } else if (strcmp(name, "hide_status_bar") == 0) {
      m_config.hideStatusBar = strcmp(value, "true") == 0;
    } else if (strcmp(name, "relay_upper_flags") == 0) {
      m_config.relayFlags[0] = atoi(value);
    } else if (strcmp(name, "relay_lower_flags") == 0) {
      m_config.relayFlags[1] = atoi(value);
    }
    return 1;
  }

  void handleRelayMessage(int relay, MQTTAsync_message* msg) {
    bool state;
    if (processStatePayload((const char*)msg->payload, msg->payloadlen, state)) {
      m_relay.setRelay(relay, state);
    }
  }

  void handleScreenMessage(MQTTAsync_message* msg) {
    bool state;
    if (processStatePayload((const char*)msg->payload, msg->payloadlen, state)) {
      m_relay.setScreen(state);
    }
  }

  void start() {
    // parse config
    if (ini_parse("/sdcard/wink_manager.ini", _configHandler, this) < 0) {
      printf("Can't load /sdcard/wink_manager.ini\n");
      exit(EXIT_FAILURE);
    }

    m_messageCallbacks.emplace(m_config.mqttTopicPrefix + "/relays/0", std::bind(&WinkRelayManager::handleRelayMessage, this, 0, std::placeholders::_1));
    m_messageCallbacks.emplace(m_config.mqttTopicPrefix + "/relays/1", std::bind(&WinkRelayManager::handleRelayMessage, this, 1, std::placeholders::_1));
    m_messageCallbacks.emplace(m_config.mqttTopicPrefix + "/screen", std::bind(&WinkRelayManager::handleScreenMessage, this, std::placeholders::_1));

    if (m_config.hideStatusBar) {
      system("service call activity 42 s16 com.android.systemui");
    }

    MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
    MQTTAsync_create(&m_mqttClient, m_config.mqttAddress.c_str(), m_config.mqttClientId.c_str(), MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTAsync_setCallbacks(m_mqttClient, this, NULL, _messageArrived, NULL);
    MQTTAsync_setConnected(m_mqttClient, this, _onConnected);

    conn_opts.keepAliveInterval = 10;
    conn_opts.cleansession = 1;
    conn_opts.onFailure = _onConnectFailure;
    conn_opts.context = this;
    conn_opts.automaticReconnect = 1;
    if (!m_config.mqttUsername.empty()) {
      conn_opts.username = m_config.mqttUsername.c_str();
    }
    if (!m_config.mqttPassword.empty()) {
      conn_opts.password = m_config.mqttPassword.c_str();
    }

    int rc;
    if ((rc = MQTTAsync_connect(m_mqttClient, &conn_opts)) != MQTTASYNC_SUCCESS)
    {
      printf("Can't connect to %s - rcode %d\n", m_config.mqttAddress.c_str(), rc);
      exit(EXIT_FAILURE);
    }

    m_relay.setCallbacks(this);
    m_relay.start(false);
    MQTTAsync_destroy(&m_mqttClient);
  }
};

int main(void) {
  WinkRelayManager manager;
  manager.start();
  return 0;
}

void _onConnectFailure(void* context, MQTTAsync_failureData* response) {
  ((WinkRelayManager*)context)->onConnectFailure(response);
}

void _onConnected(void* context, char* cause) {
  ((WinkRelayManager*)context)->onConnected(cause);
}

int _messageArrived(void* context, char* topicName, int topicLen, MQTTAsync_message* message) {
  ((WinkRelayManager*)context)->messageArrived(topicName, topicLen, message);
  return true;
}

int _configHandler(void* user, const char* section, const char* name, const char* value) {
  return ((WinkRelayManager*)user)->handleConfigValue(section, name, value);
}