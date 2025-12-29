# Eyelash Corne ZMK Keymap

## Keymap Diagram

![Diagram](https://github.com/haShinui/MAIN_zmk/blob/Mymain/keymap-drawer/eyelash_corne.svg)

---

## ⚡ Features
- ✅ **Layered keymap** with Home Row Mods (HRM)
- ✅ **Unicode support** for German (Ä, Ö, Ü, ß) and French characters
- ✅ **Bluetooth layer switching**
- ✅ **Scroll and RGB encoder support**

---

## 🛠️ **How to Modify the Keymap**
1. **Edit `config/eyelash_corne.keymap`** to adjust key bindings.
2. **Build firmware with ZMK**:
   First build **with** german.dtsi so makes firmware
3. **Remove german.dtsi**
    So that it can make new drawing and display it, takes some time to update, cached in browser.
3. **Add german.dtsi back**
    For next time

### *Note*
When working with [Keymap editor](https://nickcoutsos.github.io/keymap-editor/), after pushing you need to **reconfigure .keymap** file and put **german.dtsi** at the bottom of the other includes
example

### **Costum SVG** 
Should be done with [this](https://pixelied.com/home). Here can combine 2 svg in 1.
```
#define HOST_OS 1
#include <behaviors.dtsi>
#include "zmk-helpers/helper.h"
#include <dt-bindings/zmk/bt.h>
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/mouse.h>
#include <dt-bindings/zmk/pointing.h>
#include <dt-bindings/zmk/outputs.h>
#include <dt-bindings/zmk/rgb.h>

#include <input/processors.dtsi>

#include "german.dtsi"
```
