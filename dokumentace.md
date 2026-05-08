# SP1 – Průměrné teploty

## 1. Analýza problému

Program načítá dvě CSV datové sady (stanice, měření) a provádí nad nimi sérii transformací:

1. **Parsování** – načtení ~500 stanic a ~4,7 M měření z disku
2. **Filtrování** – odstranění stanic nesplňujících kritéria (≥ 5 po sobě jdoucích let, ≥ 100 hodnot/rok)
3. **Analýza** – výpočet měsíčních průměrů a detekce meziročních výkyvů
4. **Výstup** – 12 SVG map + `vykyvy.csv`

Jednotlivé kroky jsou sekvenčně závislé (každý závisí na výsledku předchozího), ale **uvnitř každého kroku** jsou iterace vzájemně nezávislé – to je hlavní prostor pro paralelizaci.

---

## 2. Identifikovaná místa pro paralelizaci

- **`parseMeasurements`** – parse smyčka přes řádky (`parallel for` se zápisem za pomocí známého indexu)
- **`filterStations`** – validační check per stanice (`parallel for` přes dynamické pole stanic)
- **`computeMonthlyAverages`** – akumulace měření (thread-local mapy)
- **`detectFluctuations`** – akumulace + analýza per (stanice, měsíc) (thread-local mapy + `parallel for dynamic`)
- **global min/max** – redukce přes měření (thread-local pole + `min/max_element`)
- **`generateSVGs`** – generování 12 SVG souborů (`parallel for` přes měsíce)

---

## 3. Precedenční graf

```
┌─────────────────────────────────┐
│         parseData               │  (I/O – sekvenční)
│  parseStations + parseMeasure.  │
└────────────────┬────────────────┘
                 │
┌────────────────▼────────────────┐
│        filterStations           │  (agregace sér. + validace par.)
└────────────────┬────────────────┘
                 │
      ┌──────────┴──────────┐
      │                     │
┌─────▼──────┐   ┌──────────▼──────────┐
│ global     │   │ computeMonthly      │
│ min/max    │   │ Averages            │  (paralelní)
│ (paralelní)│   └──────────┬──────────┘
└─────┬──────┘              │
      │            ┌────────▼────────┐
      │            │ detectFluctuat. │  (paralelní)
      │            └────────┬────────┘
      │                     │
      └──────────┬──────────┘
                 │
      ┌──────────┴──────────┐
      │                     │
┌─────▼──────┐   ┌──────────▼──────┐
│generateSVGs│   │  write          │
│(paralelní) │   │  vykyvy.csv     │
└────────────┘   └─────────────────┘
```

### Kritická cesta

`parseData` → `filterStations` → `computeMonthlyAverages` → `generateSVGs`

Nejdelší sekvenční závislost – určuje dolní mez doby běhu bez ohledu na počet jader.

---

## 4. Teoretické urychlení (Amdahlův zákon)

### Odhad sériové frakce

| Část programu | Odhad podílu z celk. času | Paralelizovatelná část |
|---|---|---|
| Disk I/O (čtení souborů) | ~30 % | 0 % |
| parseMeasurements (parse) | ~20 % | ~90 % |
| filterStations | ~15 % | ~60 % |
| computeMonthlyAverages | ~15 % | ~85 % |
| detectFluctuations | ~15 % | ~85 % |
| generateSVGs + výstup | ~5 % | ~100 % |

Vážený odhad sériové frakce **s ≈ 0,35** (dominuje disk I/O a critical sections).

### Amdahlovo urychlení S(p) = 1 / (s + (1−s)/p)

| Počet jader p | Teoretické S(p) |
|---|---|
| 2 | 1,54 |
| 4 | 1,86 |
| 8 | 2,10 |
| 16 | 2,24 |
| ∞ | 2,86 (maximum) |

---

## 5. Naměřené výsledky

Měřeno na Debug buildu, příkazy pro PowerShell:
```powershell
$env:OMP_NUM_THREADS=1; .\build\Debug\upp_sp1.exe data\stanice.csv data\mereni.csv --serial
$env:OMP_NUM_THREADS=2; .\build\Debug\upp_sp1.exe data\stanice.csv data\mereni.csv --parallel
$env:OMP_NUM_THREADS=4; .\build\Debug\upp_sp1.exe data\stanice.csv data\mereni.csv --parallel
$env:OMP_NUM_THREADS=8; .\build\Debug\upp_sp1.exe data\stanice.csv data\mereni.csv --parallel
```

### Naměřené časy (Debug build)

| Konfigurace | Čas [s] |
|---|---|
| `--serial` (1 vlákno) | 2.76392 |
| `--parallel` (2 vlákna) | 1.81510 |
| `--parallel` (4 vlákna) | 1.35911 |
| `--parallel` (8 vláken) | 1.25232 |

### Urychlení S(p) = T(1) / T(p)

| p | Naměřené S(p) | Amdahl S(p) při s=0,35 |
|---|---|---|
| 2 | 1,52 | 1,48 |
| 4 | 2,03 | 1,95 |
| 8 | 2,21 | 2,32 |

### Efektivita E(p) = S(p) / p

| p | E(p) |
|---|---|
| 2 | 0,76 |
| 4 | 0,51 |
| 8 | 0,28 |

### Gustafsonova metrika S(p) = p − s·(p−1)

| p | Gustafson S(p) při s=0,35 |
|---|---|
| 2 | 1,65 |
| 4 | 2,95 |
| 8 | 5,55 |

### Porovnání s očekáváním

Naměřené urychlení (1,52× pro p=2, 2,03× pro p=4) odpovídá Amdahlovým předpovědím pro s≈0,35. Při p=8 je naměřené urychlení (2,21×) mírně nižší než Amdahlova predikce (2,32×).

Efektivita klesá s počtem vláken (0,76 → 0,51 → 0,28), což je očekávané chování .

---

## 6. Konfigurace testovacího PC

| Položka | Hodnota |
|---|---|
| CPU | AMD Ryzen 5 9600X |
| Počet jader / vláken | 6 jader / 12 vláken |
| RAM | 32 GB DDR5 |
| OS | Windows 11 |
| Kompilátor | MSVC 19.x (Visual Studio 2022) |
| Optimalizace | Release (`/O2`) |
