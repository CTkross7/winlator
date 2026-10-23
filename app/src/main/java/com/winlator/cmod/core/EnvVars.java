package com.winlator.cmod.core;

import androidx.annotation.NonNull;

import java.util.Iterator;
import java.util.LinkedHashMap;

public class EnvVars implements Iterable<String> {
    private final LinkedHashMap<String, String> data = new LinkedHashMap<>();

    public EnvVars() {}

    public EnvVars(String values) {
        putAll(values);
    }

    public void put(String name, Object value) {
        data.put(name, String.valueOf(value));
    }

    public void putAll(String values) {
        if (values == null || values.isEmpty()) return;
        String[] parts = values.split(" ");
        String lastName = null;
        for (String part : parts) {
            int index = part.indexOf("=");

            // A value may contain spaces (e.g. WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--no-sandbox --disable-gpu"),
            // which toString() writes unescaped. A token without "=" or starting with "-" continues the previous value.
            if (lastName != null && (index < 0 || part.startsWith("-"))) {
                data.put(lastName, data.get(lastName)+" "+part);
                continue;
            }

            if (index < 0) continue;

            String name = part.substring(0, index);
            String value = part.substring(index+1);
            data.put(name, value);
            lastName = name;
        }
    }

    public void putAll(EnvVars envVars) {
        data.putAll(envVars.data);
    }

    public String get(String name) {
        return data.getOrDefault(name, "");
    }

    public void remove(String name) {
        data.remove(name);
    }

    public boolean has(String name) {
        return data.containsKey(name);
    }

    public void clear() {
        data.clear();
    }

    public boolean isEmpty() {
        return data.isEmpty();
    }

    public String print() {
        return String.join(", ", toStringArray());
    }

    @NonNull
    @Override
    public String toString() {
        return String.join(" ", toStringArray());
    }

    public String toEscapedString() {
        String result = "";
        for (String key : data.keySet()) {
            if (!result.isEmpty()) result += " ";
            String value = data.get(key);
            result += key+"="+value.replace(" ", "\\ ");
        }
        return result;
    }

    public String[] toStringArray() {
        String[] stringArray = new String[data.size()];
        int index = 0;
        for (String key : data.keySet()) stringArray[index++] = key+"="+data.get(key);
        return stringArray;
    }

    @NonNull
    @Override
    public Iterator<String> iterator() {
        return data.keySet().iterator();
    }
}
