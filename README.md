# PowerSzaro

**PowerSzaro** to niezwykle szybki i wydajny program konsolowy napisany w czystym języku C, przeznaczony do konwersji 24-bitowych obrazów BMP do skali szarości. Narzędzie oferuje zaawansowany silnik przetwarzania z optymalizacją wektorową **AVX2**, zestaw filtrów postprocessingu oraz możliwość zapisu do ultra-lekkiego formatu PGM.

Wszystko to z zachowaniem pełnej czystości etycznej – oprogramowanie jest w 100% Open Source!

---

## 🚀 Główne Cechy

* **Wektorowa Wydajność (AVX2):** Wykorzystanie instrukcji SIMD do przetwarzania wielu pikseli jednocześnie. Program automatycznie przełącza się na bezpieczny fallback skalarny, jeśli procesor nie wspiera AVX2 lub szerokość obrazu mogłaby coś *zepsuć koncertowo*.
* **Brak Operacji Zmiennoprzecinkowych:** Cały jargon matematyczny i współczynniki kolorów zostały przeskalowane bitowo (fixed-point math poprzez bit-shift), co zapewnia potężny wzrost prędkości na poziomie procesora.
* **Format PGM (--min-size):** Możliwość zapisu do formatu Netpbm P5 (PGM), który nie posiada zbędnego paddingu ani palet, generując pliki nawet ~4x mniejsze niż klasyczny BMP.
* **Efekt „Niby-CRT”:** Unikalny filtr stylizujący obraz na retro monitor kineskopowy poprzez naprzemienne nakładanie linii skanujących (15% przyciemnienia co drugiego piksela).
* **Kompletny Postprocessing:** Wbudowana korekcja jasności, binaryzacja (progowa czarno-biała) oraz generowanie negatywu.

---

## 🛠️ Kompilacja

Do skompilowania programu wymagany jest kompilator wspierający standard C99 oraz instrukcje AVX2 (np. GCC lub Clang).

```bash
make
```
Program dystrybuowany jest na licencji GPL 3.0, jest w zakładce po prawej opisana jako "GPL-3.0 license".
