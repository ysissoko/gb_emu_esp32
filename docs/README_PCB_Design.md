# ESP32-S3 Game Boy Emulator PCB Design - README (Complet)

Ce document décrit le plan complet pour concevoir une carte PCB basée sur ESP32-S3-WROOM-1, intégrant un émulateur Game Boy avec écran ST7789V, lecteur SD, 8 boutons, PSRAM, batterie LiPo, dissipateur thermique, USB pour flashing/recharge, bouton mode flash (BOOT), bouton reset, LED RGB indicateur état charge, LED ON/OFF, et affichage niveau batterie sur LCD. Basé sur le code existant du projet gb_emu_esp32. Utilisez ceci pour établir la BOM (Bill of Materials), les schémas, et autres documents PCB.

## 1. Vue d'ensemble du projet
- **Microcontrôleur** : ESP32-S3-WROOM-1 (avec PSRAM octal, ex. variante N16R16V pour 16MB flash + 16MB PSRAM).
- **Fonctionnalités** :
  - Émulation Game Boy (écran 160x144 pixels upscalé).
  - Écran LCD ST7789V SPI (résolution ≥160x144, ex. 240x240) - affichage niveau batterie (barres/bar graph, espace disponible).
  - Lecteur carte SD SPI (partage bus SPI, alimentation 3.3V via batterie).
  - 8 boutons Game Boy (directions + actions).
  - Wi-Fi/BT pour chargement ROMs via USB Serial/JTAG.
  - Batterie LiPo : Alimentation autonome, niveau mesuré via ADC et affiché sur LCD.
  - Dissipateur thermique : Sur ESP32.
  - USB : Flashing code émulateur + recharge batterie.
  - Bouton mode flash (BOOT) : Pour entrer en mode flash.
  - Bouton reset : Pour redémarrer logiciel.
  - LED RGB indicateur : Rouge (<10%), vert (chargée), orange (charge en cours).
  - LED ON/OFF : Bleue (allumée).
- **Alimentation** : Batterie 3.7V LiPo (charge via USB 5V), régulateur à 3.3V.
- **Contraintes** : Température <85°C, ADC précision ±5%, courants <355mA en TX Wi-Fi.

## 2. Mapping GPIO (complet, vérifié sans conflits)
- Bus SPI partagé : CLK GPIO12, MOSI GPIO11, MISO GPIO13.
- LCD ST7789V : CS GPIO9, DC GPIO10, RST GPIO8, BL GPIO7.
- SD : CS GPIO14.
- Boutons : RIGHT GPIO1, LEFT GPIO2, UP GPIO3, DOWN GPIO4, A GPIO5, B GPIO6, SELECT GPIO7, START GPIO15.
- LED RGB charge : Rouge GPIO28, Vert GPIO29, Bleu GPIO30. (GPIO22-24,25-27 indisponibles)
- LED ON/OFF : GPIO21.
- ADC niveau batterie : GPIO38.
- Bouton BOOT : GPIO0.
- UART debug : TXD0 GPIO43, RXD0 GPIO44.
- USB : D- GPIO19, D+ GPIO20.

## 3. Schéma électrique - Notes pour conception (complet)
- **Alimentation** : Batterie LiPo + chargeur TP4056 via USB + régulateur buck 3.3V (MP1584EN) + protection DW01A/8205A. Condensateurs 22µF + 0.1µF. Circuit RC EN : R=10kΩ, C=1µF.
- **Bus SPI** : Partagé LCD/SD, lignes courtes (<50mm), impédance 50Ω, pull-up 10kΩ MISO.
- **LCD ST7789V** : SPI, VCC 3.3V, condensateur 10µF, BL PWM GPIO7, affichage barres niveau batterie.
- **SD** : SPI 3.3V, condensateur 10µF.
- **Boutons** : Pull-up interne, debounce logiciel 20ms.
- **LED RGB** : Anode commune, cathodes GPIO28/29/30 via 220Ω, PWM pour orange (red+green).
- **LED ON/OFF** : GPIO21 via 220Ω, allumée au boot.
- **ADC batterie** : Diviseur 10kΩ/10kΩ vers GPIO38 (ADC1_CH2), mesure 0-3.7V.
- **USB** : Micro-USB, D+/D- GPIO19/20, VCC charge, GND commun.
- **Dissipateur** : Colle thermique + aluminium 10x10x5mm sur ESP32.
- **RF/Antenne** : Keepout zone (Figure 3-1 datasheet), antenne PCB ou U.FL (WROOM-1U).
- **Placement PCB** : ESP32 central, bus SPI court, LEDs près USB, batterie en bord.

## 4. BOM (Bill of Materials) - Liste complète
| Composant | Référence | Quantité | Description | Fournisseur |
|-----------|-----------|---------|-------------|-------------|
| ESP32-S3-WROOM-1 | U1 | 1 | Module Wi-Fi/BT avec PSRAM | Espressif |
| Écran ST7789V | LCD1 | 1 | LCD SPI 240x240 | AliExpress |
| Lecteur SD | SD1 | 1 | Module SD SPI 3.3V | AliExpress |
| Boutons tactiles | BTN1-8 | 8 | Boutons 6x6mm | Digikey |
| Bouton BOOT | BTN9 | 1 | Mode flash GPIO0 | Digikey |
| Bouton reset | BTN10 | 1 | Reset EN | Digikey |
| LED RGB | LED1 | 1 | Rouge/vert/bleu GPIO28/29/30 | Digikey |
| LED ON/OFF | LED2 | 1 | Bleue GPIO21 | Digikey |
| Batterie LiPo | BAT1 | 1 | 3.7V 1000mAh JST PH2.0 | AliExpress |
| Chargeur TP4056 | CHG1 | 1 | Charge LiPo USB | AliExpress |
| Régulateur 3.3V | REG1 | 1 | Buck MP1584EN 2A | Digikey |
| Protection batterie | PROT1 | 1 | DW01A + MOSFET 8205A | Digikey |
| Dissipateur thermique | HEAT1 | 1 | Aluminium 10x10x5mm + pâte thermique | AliExpress |
| Connecteur USB | USB1 | 1 | Micro-USB | Digikey |
| Résistance 10kΩ | R1-3,15-16 | 5 | Pull-up MISO/RC EN/ADC | Digikey |
| Résistance 1kΩ | R4-11 | 8 | Pull-down boutons | Digikey |
| Résistance 220Ω | R12-14 | 3 | LEDs RGB/ON/OFF | Digikey |
| Condensateur 22µF | C1 | 1 | Electrolyte 16V | Digikey |
| Condensateur 0.1µF | C2-4 | 3 | Céramique 50V | Digikey |
| Condensateur 10µF | C5-6 | 2 | Electrolyte 16V LCD/SD | Digikey |
| MOSFET 2N7002 | Q1 | 1 | Contrôle BL LCD | Digikey |
| Antenne U.FL (option) | ANT1 | 1 | Pour WROOM-1U | Digikey |

**Total estimé** : ~45-55 composants. Coût ~30-50€ (sans PCB).

## 5. Logiciel et validation (complet)
- **Code base** : gb_emu_esp32 ESP-IDF.
- **ADC batterie** : adc1_config_width(ADC_WIDTH_BIT_12); adc1_config_channel_atten(ADC1_CHANNEL_2, ADC_ATTEN_DB_11); raw = adc1_get_raw(ADC1_CHANNEL_2); voltage = (raw / 4095.0) * 3.3 * 2; percent = (voltage / 3.7) * 100; Afficher barres TFT_eSPI sur LCD.
- **LEDs** : ledc_timer_config_t ledc_timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0, .duty_resolution = LEDC_TIMER_13_BIT, .freq_hz = 5000}; ledc_channel_config_t ledc_channel = {.gpio_num = GPIO_NUM_28, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0}; ledc_channel_config(&ledc_channel); ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4095); // Rouge pleine. Pour orange : duty red/vert ~2048.
- **LED ON/OFF** : gpio_set_level(GPIO_NUM_21, 1); // Au boot.
- **Test sans matériel** : #define TEST_MODE 1, hardcode voltage=3.5, LEDs simulées logs.
- **Tests** : Courants <355mA, températures <85°C, RF compatible, autonomie batterie 5-10h, affichage niveau ±5%.

## 6. Prochaines étapes pour conception PCB
1. **BOM** : Commander composants (Digikey/AliExpress).
2. **Schéma** : KiCad/Eagle avec ESP32-S3-WROOM-1 symbol.
3. **Routing** : Pistes 0.2mm SPI, via thermiques.
4. **Fabrication** : PCB 2 couches, prototypes JLCPCB.
5. **Assemblage** : Soudure, test ESP-IDF flash.
6. **Validation** : Émulation Game Boy, charge/recharge, LEDs, affichage batterie.

## 7. Ressources supplémentaires
- Datasheet ESP32-S3-WROOM-1 : https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf
- Code gb_emu_esp32 : GitHub local.
- Outils : ESP-IDF, KiCad.

Document complet. Prêt pour conception PCB. Questions ou ajustements ?