🇬🇧 [English](../development.md) | 🇹🇷 Türkçe

# diyDaw'ı Geliştirmek

Bu belge diyDaw'ı kaynak koddan derlemeyi, testleri çalıştırmayı, yardımcı araçları ve kod
üzerinde çalışma kurallarını anlatır. Programın iç yapısı için [mimari.md](mimari.md), proje
dosyası için [dosya-formati.md](dosya-formati.md) belgesine bakın.

**İçindekiler**

1. [Gereksinimler](#1-gereksinimler)
2. [Kaynak kodu almak](#2-kaynak-kodu-almak)
3. [Derleme](#3-derleme)
4. [Çalıştırma ve komut satırı seçenekleri](#4-çalıştırma-ve-komut-satırı-seçenekleri)
5. [Testler](#5-testler)
6. [Arayüzü pencere açmadan kontrol etmek (snapshot)](#6-arayüzü-pencere-açmadan-kontrol-etmek-snapshot)
7. [Kod kuralları](#7-kod-kuralları)
8. [Örnek: yeni bir efekt eklemek](#8-örnek-yeni-bir-efekt-eklemek)
9. [Örnek: modele yeni bir düzenleme eklemek](#9-örnek-modele-yeni-bir-düzenleme-eklemek)
10. [Masaüstü entegrasyonu](#masaüstü-entegrasyonu)
11. [Çalışma düzeni: paralel ajanlar](#11-çalışma-düzeni-paralel-ajanlar)
12. [Sık karşılaşılan derleme sorunları](#12-sık-karşılaşılan-derleme-sorunları)

---

## 1. Gereksinimler

- **Linux** (Ubuntu 22.04 / 24.04 ve türevlerinde denendi)
- **C++20** derleyici: GCC 11+ ya da Clang 14+
- **CMake** 3.22+ ve **Ninja**
- **Python 3** (sadece bazı yardımcı betikler için, zorunlu değil)
- JUCE'un Linux bağımlılıkları:

```bash
sudo apt install build-essential cmake ninja-build \
    libasound2-dev libjack-jackd2-dev \
    libfreetype-dev libfontconfig1-dev \
    libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
    libxinerama-dev libxrandr-dev libxrender-dev libxi-dev \
    libegl-dev libglu1-mesa-dev mesa-common-dev
```

`libcurl` ve `webkit2gtk` gerekmez: diyDaw `JUCE_USE_CURL=0` ve `JUCE_WEB_BROWSER=0` ile derlenir.
Eski sistemlerde `libfreetype-dev` yerine `libfreetype6-dev` kullanın.

## 2. Kaynak kodu almak

JUCE 9.0.6 bir git submodule'üdür (`external/JUCE`):

```bash
git clone --recursive https://github.com/barlasardic/diyDaw.git
# ya da submodule'süz klonladıysanız:
git submodule update --init
```

`external/JUCE` içindeki dosyalar düzenlenmez.

## 3. Derleme

```bash
# Tam derleme (uygulama + testler + test eklentileri), hata ayıklama modunda
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

İlk derleme JUCE'u da derlediği için birkaç dakika sürer; sonrakiler sadece değişen dosyaları
derler.

### CMake seçenekleri

| Seçenek | Varsayılan | Açıklama |
| --- | --- | --- |
| `DAW_BUILD_APP` | ON | Arayüz uygulamasını derle |
| `DAW_BUILD_TESTS` | ON | `DawEngineTests` test programını derle |
| `DAW_BUILD_TEST_PLUGINS` | ON | Eklenti testleri için iki küçük VST3/LV2 eklentisini derle |
| `DAW_AUDIO_BACKENDS` | ON | ALSA ve JACK ses çıkışı. `OFF`: ses kartı desteği olmadan derler (ALSA/JACK paketleri kurulu olmayan makinelerde derleme kontrolü için) |

Örnekler:

```bash
# Ses kartı paketleri olmadan, sadece derleme kontrolü
cmake -S . -B build-noaudio -G Ninja -DDAW_AUDIO_BACKENDS=OFF

# Sadece testler (arayüz yok)
cmake -S . -B build-tests -G Ninja -DDAW_BUILD_APP=OFF -DDAW_AUDIO_BACKENDS=OFF

# Optimize edilmiş sürüm
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
```

### Hedefler (targets)

| Hedef | Çıktı |
| --- | --- |
| `DiyDaw` | `build/DiyDaw_artefacts/Debug/diyDaw` |
| `DawEngineTests` | `build/DawEngineTests_artefacts/Debug/DawEngineTests` |
| `DiyDawTestGain_VST3`, `DiyDawTestGain_LV2`, `DiyDawTestSynth_VST3`, `DiyDawTestSynth_LV2` | `build/tests/plugins/DiyDawTest*_artefacts/Debug/{VST3,LV2}/` |

Kaynak listeleri `CMakeLists.txt` içinde elle tutulur: `daw_engine` (engine/*.cpp), `daw_model`
(model/*.cpp), testler ve uygulama. Yeni bir `.cpp` dosyası eklediğinizde ilgili listeye ekleyin.

## 4. Çalıştırma ve komut satırı seçenekleri

```bash
./build/DiyDaw_artefacts/Debug/diyDaw                          # boş proje
./build/DiyDaw_artefacts/Debug/diyDaw examples/midi-demo.diydaw # bir projeyle aç
```

| Seçenek | Açıklama |
| --- | --- |
| `diyDaw <proje.diydaw>` | Projeyi açarak başla |
| `--export <çıktı.wav> <proje.diydaw>` | Pencere açmadan projenin tamamını 44,1 kHz / 24 bit WAV'a render et (otomatik efekt kuyruğuyla) ve çık |
| `--snapshot <çıktı.png> [GxY] [dosyalar...]` | Arayüzü pencere açmadan bir PNG'ye çiz ve çık (aşağıya bakın) |
| `--dialog <ad>` | `--snapshot` ile birlikte: ana pencere yerine bir paneli çiz. Adlar: `export`, `pianoroll`, `synth`, `devices`, `master`, `plugins` |
| `--show-automation` | `--snapshot` ile birlikte: otomasyonu olan track'lerin otomasyon şeritlerini aç |
| `--piano-rows <px>` / `--piano-keys <px>` | `--dialog pianoroll` ile birlikte: piano roll satır yüksekliği / klavye genişliği |
| `--plugin-path <klasör>` | O klasördeki eklentileri hemen tara (snapshot ve export için) |

Terminalde görülen `Cannot connect to server socket` satırları JACK sunucusunun çalışmadığını
söyler ve zararsızdır.

## 5. Testler

```bash
cmake --build build --target DawEngineTests
./build/DawEngineTests_artefacts/Debug/DawEngineTests
```

Program her test bölümünü listeler ve sonunda `ALL TESTS PASSED` ya da `TESTS FAILED: N` yazar;
çıkış kodu başarıda 0'dır. Sadece belirli testleri görmek için çıktıyı süzebilirsiniz:

```bash
./build/DawEngineTests_artefacts/Debug/DawEngineTests | grep -A3 "Starting tests in: Drum"
```

### Test dosyaları

| Dosya | Kapsam |
| --- | --- |
| `TransportTests` | Konum, tempo, atlama, blok sürekliliği |
| `MetronomeTests` | Tık zamanlaması (blok boyutundan bağımsız) |
| `TrackTests` | Ses clip'i çalma, fade, loop, gain/pan/mute/solo |
| `ProjectTests` | Clip düzenleme ve üst üste binme kuralları, undo |
| `PersistenceTests` | Kaydetme/açma, göreli yollar, hatalı dosyalar |
| `ExportTests` | Offline render, bit derinlikleri, normalize, iptal |
| `SynthTests` | Synth: perde doğruluğu, envelope, filtre, ses çalma, blok bağımsızlığı |
| `MidiTests` | MIDI zamanlaması, loop'lar, asılı nota olmaması, model işlemleri |
| `LiveNoteTests` | Canlı notalar (hızlı tıklama dahil) |
| `EffectsTests` | EQ, compressor, reverb, delay DSP'si |
| `DeviceChainTests` | Efekt zinciri modeli, transport döngüsü, track izolasyonu |
| `AutomationTests` | Otomasyon zarfları, hedefler, uid'ler, kaydetme |
| `MasterTailTests` | Master efektleri, export kuyruğu |
| `DrumRackTests` | Drum rack DSP'si, hazır sesler |
| `DrumTrackTests` | Drum track'leri, pad'ler, kaydetme |
| `PluginTests` | VST3 ve LV2 eklentileri: tarama, efekt, enstrüman, durum, eksik eklenti |

### Test yazma ipuçları

- Testler arayüzsüzdür; motoru `Engine::prepare()` + `renderOffline()` ile çalıştırın.
- Ses dosyası yerine bellekte bir `SampleData` oluşturup `SamplePool::insert()` ile "sanal" bir
  yola koyun (örneğin `/virtual/kick.wav`).
- Çıktıyı sample sample karşılaştırın; zamanlamayı farklı blok boyutlarıyla (1, 7, 64, 512)
  doğrulayın.
- Fader'lar 20 ms'de yumuşar: ölçümleri başlangıçtan sonra yapın ya da motoru önce durdurulmuş
  halde biraz çalıştırın.
- Eklenti testlerinde `DAW_TEST_PLUGIN_DIR` klasörünü tarayın. LV2 parametre değişiklikleri bir
  sonraki `processBlock`'ta uygulanır; durum okumadan önce bir blok render edin.
- Hata bulduğunuzda testi silmeyin: başarısız bırakın, hatayı düzeltin.

### Test eklentileri

`tests/plugins/` iki eklenti derler (JUCE `juce_add_plugin`):

- **diyDaw Test Gain:** stereo efekt; tek parametre `gain` (0–2, varsayılan 0,5).
- **diyDaw Test Synth:** enstrüman; her notaya `0,25 × velocity × level` genlikte bir sinüs çalar.

İkisi de durumlarını parametre değerleri olarak kaydeder. Bunlar sadece test içindir; ev
klasörünüzdeki eklenti klasörlerine kopyalanmazlar.

## 6. Arayüzü pencere açmadan kontrol etmek (snapshot)

Arayüz değişikliklerini pencere açmadan görmek için uygulama kendini bir PNG'ye çizebilir:

```bash
B=./build/DiyDaw_artefacts/Debug/diyDaw

# Ana pencere, 1280x620, bir projeyle ve otomasyon şeritleri açık
$B --show-automation --snapshot ana.png 1280x620 examples/midi-demo.diydaw

# Ses dosyalarıyla: i. dosya i. track'e, i. bara yerleşir
$B --snapshot sesler.png 1280x560 davul.wav bas.wav

# Paneller
$B --dialog pianoroll --snapshot pr.png 1280x320 examples/midi-demo.diydaw
$B --dialog devices   --snapshot dev.png 1280x320 examples/drums-demo.diydaw
$B --dialog master    --snapshot m.png   1280x330 proje.diydaw
$B --dialog export    --snapshot e.png   400x400  examples/midi-demo.diydaw
$B --dialog plugins   --snapshot p.png   1000x560 --plugin-path build/tests/plugins
```

Snapshot modunda arayüzün zamanlayıcısı çalışmaz; bu yüzden konum çubuğu gibi zamanlayıcıyla
güncellenen bazı göstergeler varsayılan hallerinde görünebilir. `docs/images` altındaki
görüntüler bu komutlarla üretilmiştir.

## 7. Kod kuralları

### Biçim

- JUCE stili: 4 boşlukla girinti, fonksiyon çağrılarında parantezden önce boşluk (`foo (x)`),
  süslü parantezler yeni satırda.
- Sınıflar küçük tutulur; her sınıfın başında hangi thread'in neyi çağırabileceğini söyleyen kısa
  bir açıklama vardır.
- Derleme **uyarısız** olmalıdır (JUCE'un önerilen uyarı bayraklarıyla).

### JUCE 9 notları

- Font'lar `juce::FontOptions` ile oluşturulur (`Font(float)` kurucuları kullanımdan kalktı).
- Bilerek yapılan float eşitlik karşılaştırmaları için `juce::exactlyEqual` kullanılır.
- Eski JUCE sürümlerine dair hatıralara değil, `external/JUCE/BREAKING_CHANGES.md` ve modül
  başlıklarına güvenin.

### Mimari kuralları

- `engine/` arayüz ya da cihaz başlığı içermez; sadece `juce_audio_basics` ve
  `juce_audio_formats`'a bağlıdır.
- `model/` arayüz bileşeni oluşturmaz.
- Arayüz ValueTree'ye ya da motora doğrudan yazmaz; her düzenleme `Project` metotlarından geçer ve
  her kullanıcı hareketi tek bir `beginTransaction()` ile başlar.
- Ses thread'inde bellek ayırma, kilit, dosya/konsol erişimi ve istisna yoktur (bkz.
  [mimari.md](mimari.md#3-threadler-ve-gerçek-zamanlı-kurallar)).
- Motor ya da modelde davranış değiştiren her değişiklik için `tests/` altına test eklenir.
- Bir track'in çocuklarını dolaşırken `Project::isClip()` / `isEffect()` / `isAutomationLane()`
  kullanın; PROJECT'in çocuklarını dolaşmak yerine `getNumTracks()` / `getTrack()` kullanın
  (MASTER da PROJECT'in çocuğudur).

## 8. Örnek: yeni bir efekt eklemek

Diyelim ki bir "Chorus" efekti eklemek istiyoruz:

1. **Motor:** `engine/Effects.h`'a `AudioEffect`'ten türeyen `ChorusEffect` sınıfını ekleyin:
   - `getType()` → `"chorus"`
   - `static const std::vector<ParamInfo>& infos()` ve `getParamInfos()` → parametreler (kimlik,
     ad, aralık, varsayılan, düğme ortası değeri, adımlı mı, birim)
   - `prepareEffect()`: gecikme hatları gibi tamponları burada ayırın
   - `processEffect()`: yerinde stereo işleme; bellek ayırmayın, parametreleri `getParam()` ile
     okuyun ve tık yapacak değişiklikleri yumuşatın
   - `reset()`: iç durumu temizleyin
   - kurucuda `initialiseParams()` çağırın
2. **Kayıt:** `engine/AudioEffect.cpp` içindeki `getEffectTypes()`, `getEffectDisplayName()`,
   `getEffectParamInfos()` ve `createEffect()` fonksiyonlarına `"chorus"`'u ekleyin.
3. **Testler:** `tests/EffectsTests.cpp`'ye (ya da yeni bir dosyaya) DSP testleri yazın: varsayılan
   ayarlarla beklenen davranış, uç değerlerde NaN/sonsuz olmaması, blok boyutundan bağımsızlık.
4. **Arayüz:** Ek bir şey gerekmez: cihaz zinciri paneli düğmeleri `ParamInfo` listesinden
   otomatik oluşturur; "+ Add Effect" menüsü `getEffectTypes()`'ı listeler; otomasyon hedefleri ve
   kaydetme/açma da parametre kimlikleri üzerinden kendiliğinden çalışır.

## 9. Örnek: modele yeni bir düzenleme eklemek

Örneğin "clip'in rengini değiştir":

1. `model/Project.h`'a bir özellik kimliği (`DAW_DECLARE_ID (clipColour)`) ve metot ekleyin:
   `void setClipColour (juce::ValueTree clip, juce::Colour)`.
2. Metot ağacı `&undoManager` ile değiştirsin (geri alınabilir olsun) ve girdiyi doğrulasın.
3. Değişiklik ses motorunu etkiliyorsa `valueTreePropertyChanged` içinde ilgili senkronizasyonu
   çağırın; sadece görselse gerek yok.
4. Kalıcıysa `normaliseTypes` içinde dosyadan okunan değeri doğru türe çevirin.
5. Arayüzde: `project.beginTransaction ("Change clip colour"); project.setClipColour (...);`
6. `tests/` altına: değişiklik, geri alma/yineleme ve kaydet/aç testi.
7. [dosya-formati.md](dosya-formati.md) belgesini güncelleyin.

## Masaüstü entegrasyonu

Proje dosyaları içeride XML olduğu için masaüstü ortamı onları sıradan XML gibi (örneğin
tarayıcıyla) açabilir. `.diydaw` dosyalarının çift tıklayınca diyDaw ile açılması için kullanıcı
düzeyinde bir MIME türü ve masaüstü kaydı ekleyin:

`~/.local/share/mime/packages/diydaw.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-diydaw-project">
    <comment>diyDaw project</comment>
    <sub-class-of type="application/xml"/>
    <glob pattern="*.diydaw" weight="90"/>
    <generic-icon name="audio-x-generic"/>
  </mime-type>
</mime-info>
```

`~/.local/share/applications/diydaw.desktop` (Exec satırındaki yolu kendi derlemenize göre
değiştirin):

```ini
[Desktop Entry]
Type=Application
Name=diyDaw
Comment=Ableton-style DAW
Exec="/path/to/diyDaw/build/DiyDaw_artefacts/Debug/diyDaw" %f
Icon=audio-x-generic
Terminal=false
Categories=AudioVideo;Audio;Music;
MimeType=application/x-diydaw-project;
```

Sonra:

```bash
update-mime-database ~/.local/share/mime
update-desktop-database ~/.local/share/applications
xdg-mime default diydaw.desktop application/x-diydaw-project
```

## 11. Çalışma düzeni: paralel ajanlar

Paralel çalışan katkıcılar ve kodlama ajanları için iş akışı:

- Özelliklere başlamadan önce arayüzleri (header'ları) ve derlenebilen boş gövdeleri (stub)
  tanımlayın; katkıcılar ortak bir sözleşmeye göre çalışsın.
- Her katkıcı yalnızca kendisine ayrılan dosyaları düzenlesin ve `.gitignore` kapsamındaki
  kendi derleme klasörünü kullansın (örneğin `build-contributor-a`).
- Test odaklı katkıcılar hatayı gösteren başarısız bir test ve açık bir hata raporu bıraksın;
  düzeltmeyi ilgili uygulama sorumlusu yapsın.
- Birleştirmeden önce değişiklikleri inceleyin; özellikle ses thread'inde bellek ayırma ve nesne
  ömürlerini kontrol edin.
- [AGENTS.md](../../AGENTS.md) kısa teknik rehberdir; mimari değiştiğinde güncellenir.

## 12. Sık karşılaşılan derleme sorunları

| Hata | Çözüm |
| --- | --- |
| `alsa/asoundlib.h: No such file` | `sudo apt install libasound2-dev` (ya da `-DDAW_AUDIO_BACKENDS=OFF`) |
| `jack/jack.h: No such file` | `sudo apt install libjack-jackd2-dev` |
| `X11/extensions/XInput2.h` bulunamadı | `sudo apt install libxi-dev` |
| `ft2build.h` bulunamadı | `sudo apt install libfreetype-dev libfontconfig1-dev` |
| `external/JUCE` boş | `git submodule update --init` |
| Test eklentileri bulunamıyor | `cmake --build build --target DawEngineTests` eklentileri de derler; derleme klasörünü değiştirdiyseniz CMake'i yeniden yapılandırın |
| Testlerde "JUCE Assertion failure in juce_MessageListener" | Test programının `ScopedJuceInitialiser_GUI` ile başladığından emin olun (`tests/TestMain.cpp`) |
