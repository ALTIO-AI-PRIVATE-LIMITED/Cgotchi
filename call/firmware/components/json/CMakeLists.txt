# ESP-IDF 6 moved cJSON out of IDF into espressif/cjson. This shim keeps the old
# `json` component name working for main and for esp-sr.
idf_component_register(REQUIRES espressif__cjson)
