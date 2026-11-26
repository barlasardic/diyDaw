# `.diydaw` Proje Dosyası Formatı

Bir diyDaw projesi tek bir UTF-8 **XML** dosyasıdır (uzantı `.diydaw`). Dosya, programın bellekte
tuttuğu `juce::ValueTree` ağacının doğrudan XML karşılığıdır: her düğüm bir XML elemanı, her özellik
bir XML niteliğidir (attribute). Bu belge format sürümü **1**'i tanımlar.

İnsan tarafından okunabilir ve elle düzenlenebilir; program açarken dosyayı doğrular ve onarır
(bkz. [Açarken uygulanan kurallar](#açarken-uygulanan-kurallar)).

## Genel yapı

```
PROJECT
├── TRACK  (0..16 adet, çalma sırasıyla)
│   ├── CLIP   (audio track'te ses clip'leri, MIDI track'te MIDI clip'leri)
│   │   └── NOTE (sadece MIDI clip'lerde)
│   ├── DEVICE (MIDI track'te önce enstrüman, sonra efektler; audio track'te sadece efektler)
│   └── AUTOMATION
│       └── POINT
└── MASTER (her zaman son eleman)
    └── DEVICE (master efektleri)
```

Sayılar ondalık noktayla yazılır. `double` değerler, okunduğunda bit bit aynı değeri verecek en kısa
biçimde yazılır (örneğin `1.8765432109876543`); bu sayede kaydetme/açma clip'leri kaydırmaz.
Mantıksal değerler `1` / `0` olarak yazılır.

## PROJECT

| Nitelik | Tür | Açıklama |
| --- | --- | --- |
| `version` | tamsayı | Format sürümü. Şu an `1`. Programın bildiğinden büyükse dosya açılmaz |
| `bpm` | sayı | Tempo, 20–999 (varsayılan 120) |
| `beatsPerBar` | tamsayı | Bar başına vuruş, 1–32 (varsayılan 4) |
| `masterGain` | sayı | Master fader, dB, −60…+6 (varsayılan 0) |

## TRACK

| Nitelik | Tür | Açıklama |
| --- | --- | --- |
| `kind` | `audio` / `midi` | Track türü. Yoksa `audio`. Drum track'ler de `midi`'dir |
| `name` | metin | Görünen ad. Boşsa "N Audio" verilir |
| `colour` | ARGB onaltılık | Örneğin `ffff8a3d`. Yoksa paletten verilir |
| `gain` | sayı | Fader, dB, −60…+6 (varsayılan 0) |
| `pan` | sayı | −1 (sol) … +1 (sağ) (varsayılan 0) |
| `mute` | 0/1 | 1 = susturulmuş |
| `solo` | 0/1 | 1 = solo |
| `height` | tamsayı | Arrangement'taki yükseklik, piksel, 40–320 (varsayılan 64) |

`engineIndex` niteliği programın çalışırken kullandığı bir bilgidir ve dosyaya yazılmaz.

## CLIP (ses)

Audio track'lerde bulunur. `kind` niteliği `audio` ya da yoktur.

| Nitelik | Tür | Açıklama |
| --- | --- | --- |
| `kind` | `audio` | |
| `name` | metin | Görünen ad (genelde dosya adı) |
| `file` | yol | Ses dosyasının tam yolu |
| `relativeFile` | yol | Aynı dosyanın proje dosyasının klasörüne göre yolu (sadece dosyada; açarken kullanılır, sonra silinir) |
| `start` | sayı | Zaman çizelgesindeki başlangıç, beat (≥ 0) |
| `length` | sayı | Uzunluk, beat (≥ 1/16) |
| `offset` | sayı | Clip'in ilk sample'ının ses dosyasındaki yeri, **saniye** |
| `loop` | 0/1 | 1 = ses dosyası clip boyunca tekrar eder |

Ses clip'leri doğal hızda çalar: zaman çizelgesindeki 1 saniye, dosyadaki 1 saniyedir.

## CLIP (MIDI)

MIDI track'lerde bulunur.

| Nitelik | Tür | Açıklama |
| --- | --- | --- |
| `kind` | `midi` | |
| `name` | metin | Görünen ad (varsayılan "MIDI Clip") |
| `start` | sayı | Zaman çizelgesindeki başlangıç, beat |
| `length` | sayı | Uzunluk, beat |
| `offsetBeats` | sayı | Clip'in sol kenarının içerik zamanındaki yeri, beat |
| `loop` | 0/1 | 1 = içerik `loopLength` aralıklarla tekrar eder |
| `loopLength` | sayı | Loop uzunluğu, beat (yoksa `length`) |

**İçerik zamanı:** Zaman çizelgesindeki `t` konumunda çalınan içerik zamanı
`offsetBeats + (t − start)`'tır. Loop açıksa bu değer `loopLength`'e göre sarar (döngü içerikte
`[0, loopLength)` aralığıdır) ve loop sonunu aşan notalar orada kesilir. Loop kapalıysa içerik
düz akar.

### NOTE

| Nitelik | Tür | Açıklama |
| --- | --- | --- |
| `pitch` | tamsayı | MIDI nota numarası 0–127 (60 = C3) |
| `start` | sayı | İçerik zamanındaki başlangıç, beat (≥ 0) |
| `length` | sayı | Uzunluk, beat (≥ 1/64) |
| `velocity` | sayı | 0–1 |

Notaların dosyada sıralı olması gerekmez.

## DEVICE

Ortak nitelikler:

| Nitelik | Tür | Açıklama |
| --- | --- | --- |
| `type` | metin | Cihaz türü (aşağıdaki tablolar) |
| `uid` | tamsayı | Projede benzersiz kimlik; otomasyon hedefleri buna başvurur |
| `enabled` | 0/1 | Sadece efektlerde: 0 = bypass (varsayılan 1) |
| (parametreler) | sayı | Her parametre kendi kimliğiyle bir nitelik olarak yazılır. Eksik olanlar varsayılan değeri alır |

Bir MIDI track'te ilk DEVICE enstrümandır (`synth`, `drumrack` ya da `pluginInstrument`); her
track'te sonrakiler sırayla efektlerdir. Audio track'lerde ve MASTER'da enstrüman yoktur.

### `synth` parametreleri

| Kimlik | Aralık | Varsayılan |
| --- | --- | --- |
| `osc1Wave`, `osc2Wave` | 0 = saw, 1 = square, 2 = triangle, 3 = sine | 0 |
| `osc2Semitones` | −24…24 (tam sayı) | 0 |
| `osc2Detune` | −50…50 cent | 7 |
| `oscMix` | 0…1 | 0,5 |
| `filterCutoff` | 20…20000 Hz | 2000 |
| `filterResonance` | 0…1 | 0,2 |
| `filterEnvAmount` | −1…1 | 0,25 |
| `filterKeyTrack` | 0…1 | 0,5 |
| `filterAttack`, `filterDecay`, `filterRelease` | 0,001…10 s | 0,005 / 0,3 / 0,3 |
| `filterSustain` | 0…1 | 0,5 |
| `ampAttack`, `ampDecay`, `ampRelease` | 0,001…10 s | 0,005 / 0,3 / 0,3 |
| `ampSustain` | 0…1 | 0,7 |
| `velocitySens` | 0…1 | 0,8 |
| `volume` | −60…6 dB | 0 |

### `drumrack`

Pad başına (N = 1…16; pad 1 = MIDI 36):

| Kimlik | Aralık | Varsayılan |
| --- | --- | --- |
| `padNFile` | metin | Pad'in sample'ı: bir ses dosyasının tam yolu ya da `builtin:<ad>` |
| `padNRelativeFile` | yol | Sadece dosyada: proje klasörüne göre yol |
| `padNGain` | −60…6 dB | 0 |
| `padNPan` | −1…1 | 0 |
| `padNTune` | −24…24 yarım ton (tam sayı) | 0 |
| `padNDecay` | 0,01…10 s | 10 |
| `padNChoke` | 0…8 (0 = grup yok) | 0 |

Hazır sesler: `builtin:kick`, `rim`, `snare`, `clap`, `shaker`, `lowTom`, `closedHat`, `midTom`,
`cowbell`, `highTom`, `openHat`, `conga`, `zap`, `crash`, `sub`, `ride`. Yeni bir drum track bu
sırayla pad 1–16'ya yerleştirilmiş kitle ve `closedHat`/`openHat` choke grubu 1'de oluşturulur.
`padNFile` niteliği olmayan pad boştur.

### Efektler

| `type` | Parametre kimlikleri (aralık, varsayılan) |
| --- | --- |
| `eq` | `lowFreq` (20–1000 Hz, 100), `lowGain` (−18…18 dB, 0), `midFreq` (100–10000 Hz, 1000), `midGain` (−18…18, 0), `midQ` (0,3–8, 1), `highFreq` (1000–20000 Hz, 8000), `highGain` (−18…18, 0), `output` (−18…18, 0) |
| `compressor` | `threshold` (−60…0 dB, −18), `ratio` (1–20, 4), `attack` (0,1–100 ms, 10), `release` (10–1000 ms, 150), `knee` (0–24 dB, 6), `makeup` (0–24 dB, 0), `mix` (0–1, 1) |
| `reverb` | `size` (0–1, 0,5), `damping` (0–1, 0,5), `predelay` (0–200 ms, 10), `width` (0–1, 1), `mix` (0–1, 0,25) |
| `delay` | `sync` (0/1, 1), `syncDivision` (0–6: 1/16, 1/8, 1/8 noktalı, 1/4, 1/4 noktalı, 1/2, 1 bar; 3), `timeMs` (1–2000, 375), `feedback` (0–0,95, 0,35), `pingPong` (0/1, 0), `lowCut` (20–2000 Hz, 80), `highCut` (1000–20000 Hz, 8000), `mix` (0–1, 0,3) |

### Eklentiler: `plugin` ve `pluginInstrument`

| Nitelik | Açıklama |
| --- | --- |
| `type` | `plugin` (efekt) ya da `pluginInstrument` (MIDI track'in enstrümanı) |
| `pluginName` | Görünen ad |
| `description` | JUCE `PluginDescription`'ın tek satırlık XML hâli (format, dosya yolu, benzersiz kimlik, üretici, kategori, ...). Eklenti buna göre bulunup yüklenir |
| `state` | Eklentinin kendi durumu (`getStateInformation`), **base64**. Kaydederken yazılır |
| `enabled` | Sadece `plugin`: bypass |

Eklentinin parametreleri ayrı nitelik olarak yazılmaz; hepsi `state` içindedir. Eklenti
bulunamazsa DEVICE olduğu gibi korunur ve tekrar kaydedildiğinde `description` ile son bilinen
`state` kaybolmaz.

## AUTOMATION ve POINT

| Düğüm | Nitelik | Açıklama |
| --- | --- | --- |
| AUTOMATION | `target` | `gain`, `pan` ya da `<cihaz uid>:<parametre kimliği>` (örneğin `1:filterCutoff`, `4:mix`, `7:pad1Gain`) |
| POINT | `beat` | Zaman çizelgesindeki konum, beat (≥ 0) |
| POINT | `value` | Parametrenin kendi biriminde değer (dB, Hz, 0–1 ...) |

Değer noktalar arasında doğrusal değişir; ilk noktadan önce ilk noktanın, son noktadan sonra son
noktanın değeri geçerlidir. Noktası olmayan bir AUTOMATION çalmayı etkilemez. Hedefi artık
bulunmayan (örneğin cihazı silinmiş) bir AUTOMATION dosyada korunur ama çalmada yok sayılır.
Eklenti parametreleri şimdilik hedef olamaz.

## MASTER

Her zaman PROJECT'in son elemanıdır ve sadece efekt DEVICE'ları (dahili efektler veya `plugin`)
içerir. Yoksa açarken boş bir MASTER oluşturulur.

## Dosya yolları

Ses clip'leri ve drum pad'leri için hem tam yol (`file` / `padNFile`) hem de proje klasörüne göre
göreli yol (`relativeFile` / `padNRelativeFile`) yazılır. Açarken:

1. Tam yolda dosya varsa o kullanılır.
2. Yoksa göreli yol proje dosyasının klasörüne göre çözülür; dosya oradaysa yol güncellenir.
3. İkisi de yoksa clip/pad sessiz kalır ve arayüzde "Missing" olarak görünür.

Bu sayede bir proje klasörü (proje dosyası + yanındaki ses dosyaları) bütün olarak başka bir yere
veya bilgisayara taşınabilir. Ses dosyaları projeye kopyalanmaz.

## Açarken uygulanan kurallar

Program dosyayı açarken şunları yapar; elle yazılmış ya da bozuk dosyalar böylece güvenle açılır:

- XML'den dize olarak gelen değerleri sayıya / mantıksal değere çevirir.
- Değerleri aralıklarına sıkıştırır (tempo, gain, pan, nota perdesi ve velocity, cihaz
  parametreleri, adımlı parametrelerin yuvarlanması), negatif başlangıçları 0 yapar.
- Track türüne uymayan clip'leri (MIDI track'te ses clip'i gibi) ve tanınmayan elemanları atar.
- MIDI track'te enstrüman yoksa synth ekler; birden fazla enstrüman varsa ilkini tutar ve onu
  zincirin başına alır. Audio track'teki enstrümanları atar. Bilinmeyen efekt türlerini atar.
- Eksik ya da tekrar eden `uid`'leri yeniden numaralandırır.
- 16'dan fazla track varsa ilk 16'sını yükler.
- MASTER yoksa ekler, birden fazlaysa ilkini kullanır, sona taşır.
- Format sürümü programın bildiğinden büyükse dosyayı açmaz ve mevcut projeye dokunmaz.

## Örnek

```xml
<?xml version="1.0" encoding="UTF-8"?>
<PROJECT version="1" bpm="110" beatsPerBar="4" masterGain="0">
  <TRACK kind="midi" name="Chords" colour="ffc77aff" gain="-4" pan="0" mute="0" solo="0" height="96">
    <DEVICE type="synth" uid="1" filterCutoff="1200" ampAttack="0.4" ampRelease="0.8"/>
    <DEVICE type="reverb" uid="2" enabled="1" size="0.7" mix="0.3"/>
    <CLIP kind="midi" name="Am F C G" start="0" length="32" offsetBeats="0" loop="1" loopLength="16">
      <NOTE pitch="57" start="0" length="3.5" velocity="0.7"/>
      <NOTE pitch="60" start="0" length="3.5" velocity="0.7"/>
      <NOTE pitch="64" start="0" length="3.5" velocity="0.7"/>
    </CLIP>
    <AUTOMATION target="1:filterCutoff">
      <POINT beat="0" value="300"/>
      <POINT beat="16" value="6000"/>
    </AUTOMATION>
  </TRACK>
  <TRACK kind="midi" name="Drums" colour="ffff8a3d">
    <DEVICE type="drumrack" uid="3" pad1File="builtin:kick" pad3File="builtin:snare"
            pad7File="builtin:closedHat" pad11File="builtin:openHat" pad7Choke="1" pad11Choke="1"/>
    <CLIP kind="midi" name="Beat" start="0" length="4" offsetBeats="0" loop="1" loopLength="4">
      <NOTE pitch="36" start="0" length="0.25" velocity="1"/>
      <NOTE pitch="38" start="1" length="0.25" velocity="0.9"/>
    </CLIP>
  </TRACK>
  <TRACK kind="audio" name="Vocal">
    <CLIP kind="audio" name="vocal" file="/home/kullanici/Music/proje/vocal.wav"
          relativeFile="vocal.wav" start="8" length="16" offset="0" loop="0"/>
  </TRACK>
  <MASTER>
    <DEVICE type="compressor" uid="4" enabled="1" threshold="-20" ratio="2"/>
  </MASTER>
</PROJECT>
```

Depodaki `examples/midi-demo.diydaw` ve `examples/drums-demo.diydaw` dosyaları da elle yazılmış,
çalışan örneklerdir.

## Sürüm geçmişi

| Sürüm | Değişiklik |
| --- | --- |
| 1 | İlk format. Sonradan eklenen elemanlar (MIDI, cihazlar, otomasyon, MASTER, track yüksekliği, eklentiler) geriye uyumlu olarak eklendi: eski dosyalarda bulunmayan her şey varsayılan değerini alır |
