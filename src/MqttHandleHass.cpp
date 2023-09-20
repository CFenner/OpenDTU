// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Thomas Basler and others
 */
#include "MqttHandleHass.h"
#include "MqttHandleInverter.h"
#include "MqttSettings.h"
#include "NetworkSettings.h"

MqttHandleHassClass MqttHandleHass;

void MqttHandleHassClass::init()
{
}

void MqttHandleHassClass::loop()
{
    if (_updateForced) {
        publishConfig();
        _updateForced = false;
    }

    if (MqttSettings.getConnected() && !_wasConnected) {
        // Connection established
        _wasConnected = true;
        publishConfig();
    } else if (!MqttSettings.getConnected() && _wasConnected) {
        // Connection lost
        _wasConnected = false;
    }
}

void MqttHandleHassClass::forceUpdate()
{
    _updateForced = true;
}

void MqttHandleHassClass::publishConfig()
{
    if (!Configuration.get().Mqtt_Hass_Enabled) {
        return;
    }

    if (!MqttSettings.getConnected() && Hoymiles.isAllRadioIdle()) {
        return;
    }

    const CONFIG_T& config = Configuration.get();

    // publish DTU sensors
    publishDTUSensor("IP", "", "", "", "", "");//"diagnostic", "mdi:network-outline", "", "");
    publishDTUSensor("WiFi Signal", "signal_strength", "", "", "", "rssi");//"diagnostic", "", "dBm", "rssi");
    publishDTUSensor("Uptime", "duration", "", "", "", "");//"diagnostic", "mdi:clock-time-eight-outline", "s", "");
    publishDTUBinarySensor("Status", "connectivity");
    
    // Loop all inverters
    for (uint8_t i = 0; i < Hoymiles.getNumInverters(); i++) {
        auto inv = Hoymiles.getInverterByPos(i);

        publishInverterButton(inv, "Turn Inverter Off", "mdi:power-plug-off", "config", "", "cmd/power", "0");
        publishInverterButton(inv, "Turn Inverter On", "mdi:power-plug", "config", "", "cmd/power", "1");
        publishInverterButton(inv, "Restart Inverter", "", "config", "restart", "cmd/restart", "1");

        publishInverterNumber(inv, "Limit NonPersistent Relative", "mdi:speedometer", "config", "cmd/limit_nonpersistent_relative", "status/limit_relative", "%");
        publishInverterNumber(inv, "Limit Persistent Relative", "mdi:speedometer", "config", "cmd/limit_persistent_relative", "status/limit_relative", "%");

        publishInverterNumber(inv, "Limit NonPersistent Absolute", "mdi:speedometer", "config", "cmd/limit_nonpersistent_absolute", "status/limit_absolute", "W", 10, 2250);
        publishInverterNumber(inv, "Limit Persistent Absolute", "mdi:speedometer", "config", "cmd/limit_persistent_absolute", "status/limit_absolute", "W", 10, 2250);

        publishInverterBinarySensor(inv, "Reachable", "status/reachable", "1", "0");
        publishInverterBinarySensor(inv, "Producing", "status/producing", "1", "0");

        // Loop all channels
        for (auto& t : inv->Statistics()->getChannelTypes()) {
            for (auto& c : inv->Statistics()->getChannelsByType(t)) {
                for (uint8_t f = 0; f < DEVICE_CLS_ASSIGN_LIST_LEN; f++) {
                    bool clear = false;
                    if (t == TYPE_DC && !config.Mqtt_Hass_IndividualPanels) {
                        clear = true;
                    }
                    publishField(inv, t, c, deviceFieldAssignment[f], clear);
                }
            }
        }

        yield();
    }
}

void MqttHandleHassClass::publishDTUSensor(const char* name, const char* device_class, const char* category, const char* icon, const char* unit_of_measure, const char* subTopic)
{
    String id = name;
    id.toLowerCase();
    id.replace(" ", "_");
    String topic = subTopic;
    if (topic == "") {
        topic = id;
    }

    DynamicJsonDocument root(1024);
    root["name"] = name;
    root["stat_t"] = MqttSettings.getPrefix() + "dtu" + "/" + topic;
    root["uniq_id"] = NetworkSettings.getHostname() + "_" + id;
    root["ic"] = icon;
    root["ent_cat"] = category;
    root["unit_of_meas"] = unit_of_measure;
    
    JsonObject deviceObj = root.createNestedObject("dev");
    createDTUDeviceInfo(deviceObj);

    String buffer;
    String configTopic = "sensor/" + NetworkSettings.getHostname() + "/" + id + "/config";
    serializeJson(root, buffer);
    publish(configTopic, buffer);
}

void MqttHandleHassClass::publishDTUBinarySensor(const char* name, const char* device_class)
{
    String sensorId = name;
    sensorId.toLowerCase();
    sensorId.replace(" ", "_");
    String topic = sensorId;

    DynamicJsonDocument root(1024);
    root["name"] = name;
    root["uniq_id"] = NetworkSettings.getHostname() + "_" + sensorId;
    root["dev_cla"] = device_class;
    root["stat_t"] = MqttSettings.getPrefix() + "dtu" + "/" + topic;
    
    JsonObject deviceObj = root.createNestedObject("dev");
    createDTUDeviceInfo(deviceObj);

    String buffer;
    String configTopic = "binary_sensor/" + NetworkSettings.getHostname() + "/" + sensorId + "/config";
    serializeJson(root, buffer);
    publish(configTopic, buffer);
}

void MqttHandleHassClass::publishField(std::shared_ptr<InverterAbstract> inv, ChannelType_t type, ChannelNum_t channel, byteAssign_fieldDeviceClass_t fieldType, bool clear)
{
    if (!inv->Statistics()->hasChannelFieldValue(type, channel, fieldType.fieldId)) {
        return;
    }

    String serial = inv->serialString();

    String fieldName;
    if (type == TYPE_AC && fieldType.fieldId == FLD_PDC) {
        fieldName = "PowerDC";
    } else {
        fieldName = inv->Statistics()->getChannelFieldName(type, channel, fieldType.fieldId);
    }

    String chanNum;
    if (type == TYPE_DC) {
        // TODO(tbnobody)
        chanNum = static_cast<uint8_t>(channel) + 1;
    } else {
        chanNum = channel;
    }

    String configTopic = "sensor/dtu_" + serial
        + "/" + "ch" + chanNum + "_" + fieldName
        + "/config";

    if (!clear) {
        String stateTopic = MqttSettings.getPrefix() + MqttHandleInverter.getTopic(inv, type, channel, fieldType.fieldId);
        const char* devCls = deviceClasses[fieldType.deviceClsId];
        const char* stateCls = stateClasses[fieldType.stateClsId];

        String name;
        if (type != TYPE_DC) {
            name = String(inv->name()) + " " + fieldName;
        } else {
            name = String(inv->name()) + " CH" + chanNum + " " + fieldName;
        }

        DynamicJsonDocument root(1024);
        root["name"] = name;
        root["stat_t"] = stateTopic;
        root["uniq_id"] = serial + "_ch" + chanNum + "_" + fieldName;

        String unit_of_measure = inv->Statistics()->getChannelFieldUnit(type, channel, fieldType.fieldId);
        if (unit_of_measure != "") {
            root["unit_of_meas"] = unit_of_measure;
        }

        JsonObject deviceObj = root.createNestedObject("dev");
        if (type == TYPE_DC) {
            createStringDeviceInfo(deviceObj, inv, chanNum);
        } else {
            createInverterDeviceInfo(deviceObj, inv);
        }

        if (Configuration.get().Mqtt_Hass_Expire) {
            root["exp_aft"] = Hoymiles.getNumInverters() * max<uint32_t>(Hoymiles.PollInterval(), Configuration.get().Mqtt_PublishInterval) * inv->getReachableThreshold();
        }
        if (devCls != 0) {
            root["dev_cla"] = devCls;
        }
        if (stateCls != 0) {
            root["stat_cla"] = stateCls;
        }

        String buffer;
        serializeJson(root, buffer);
        publish(configTopic, buffer);
    } else {
        publish(configTopic, "");
    }
}

void MqttHandleHassClass::publishInverterButton(std::shared_ptr<InverterAbstract> inv, const char* caption, const char* icon, const char* category, const char* deviceClass, const char* subTopic, const char* payload)
{
    String serial = inv->serialString();

    String buttonId = caption;
    buttonId.replace(" ", "_");
    buttonId.toLowerCase();

    String configTopic = "button/dtu_" + serial
        + "/" + buttonId
        + "/config";

    String cmdTopic = MqttSettings.getPrefix() + serial + "/" + subTopic;

    DynamicJsonDocument root(1024);
    root["name"] = String(inv->name()) + " " + caption;
    root["uniq_id"] = serial + "_" + buttonId;
    if (strcmp(icon, "")) {
        root["ic"] = icon;
    }
    if (strcmp(deviceClass, "")) {
        root["dev_cla"] = deviceClass;
    }
    root["ent_cat"] = category;
    root["cmd_t"] = cmdTopic;
    root["payload_press"] = payload;

    JsonObject deviceObj = root.createNestedObject("dev");
    createInverterDeviceInfo(deviceObj, inv);

    String buffer;
    serializeJson(root, buffer);
    publish(configTopic, buffer);
}

void MqttHandleHassClass::publishInverterNumber(
    std::shared_ptr<InverterAbstract> inv, const char* caption, const char* icon, const char* category,
    const char* commandTopic, const char* stateTopic, const char* unitOfMeasure,
    int16_t min, int16_t max)
{
    String serial = inv->serialString();

    String buttonId = caption;
    buttonId.replace(" ", "_");
    buttonId.toLowerCase();

    String configTopic = "number/dtu_" + serial
        + "/" + buttonId
        + "/config";

    String cmdTopic = MqttSettings.getPrefix() + serial + "/" + commandTopic;
    String statTopic = MqttSettings.getPrefix() + serial + "/" + stateTopic;

    DynamicJsonDocument root(1024);
    root["name"] = String(inv->name()) + " " + caption;
    root["uniq_id"] = serial + "_" + buttonId;
    if (strcmp(icon, "")) {
        root["ic"] = icon;
    }
    root["ent_cat"] = category;
    root["cmd_t"] = cmdTopic;
    root["stat_t"] = statTopic;
    root["unit_of_meas"] = unitOfMeasure;
    root["min"] = min;
    root["max"] = max;

    JsonObject deviceObj = root.createNestedObject("dev");
    createInverterDeviceInfo(deviceObj, inv);

    String buffer;
    serializeJson(root, buffer);
    publish(configTopic, buffer);
}

void MqttHandleHassClass::publishInverterBinarySensor(std::shared_ptr<InverterAbstract> inv, const char* caption, const char* subTopic, const char* payload_on, const char* payload_off)
{
    String serial = inv->serialString();

    String sensorId = caption;
    sensorId.replace(" ", "_");
    sensorId.toLowerCase();

    String configTopic = "binary_sensor/dtu_" + serial
        + "/" + sensorId
        + "/config";

    String statTopic = MqttSettings.getPrefix() + serial + "/" + subTopic;

    DynamicJsonDocument root(1024);
    root["name"] = String(inv->name()) + " " + caption;
    root["uniq_id"] = serial + "_" + sensorId;
    root["stat_t"] = statTopic;
    root["pl_on"] = payload_on;
    root["pl_off"] = payload_off;

    JsonObject deviceObj = root.createNestedObject("dev");
    createInverterDeviceInfo(deviceObj, inv);

    String buffer;
    serializeJson(root, buffer);
    publish(configTopic, buffer);
}

void MqttHandleHassClass::createDTUDeviceInfo(JsonObject& object)
{
    object["name"] = NetworkSettings.getHostname();
    object["ids"] = NetworkSettings.getHostname();
    object["cu"] = String("http://") + NetworkSettings.localIP().toString();
    object["mf"] = "OpenDTU";
    object["mdl"] = "OpenDTU"; // ESP model?
    object["sw"] = AUTO_GIT_HASH;
}

void MqttHandleHassClass::createInverterDeviceInfo(JsonObject& object, std::shared_ptr<InverterAbstract> inv)
{
    object["name"] = inv->name();
    object["ids"] = inv->serialString();
    object["cu"] = String("http://") + NetworkSettings.localIP().toString();
    //object["mf"] = "";
    object["mdl"] = inv->DevInfo()->getHwModelName(); // not available ?!
    object["sw"] = inv->DevInfo()->getFwBuildVersion();
    object["via_device"] = NetworkSettings.getHostname();
}

void MqttHandleHassClass::createStringDeviceInfo(JsonObject& object, std::shared_ptr<InverterAbstract> inv, String string)
{
    object["name"] = "String " + string; // use name from config (/serial/1/name)?
    object["ids"] = inv->serialString() + "_string_" + string;
    object["cu"] = String("http://") + NetworkSettings.localIP().toString();
    object["via_device"] = inv->serialString();
}

void MqttHandleHassClass::publish(const String& subtopic, const String& payload)
{
    String topic = Configuration.get().Mqtt_Hass_Topic;
    topic += subtopic;
    MqttSettings.publishGeneric(topic.c_str(), payload.c_str(), Configuration.get().Mqtt_Hass_Retain);
}
