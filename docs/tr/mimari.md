🇬🇧 [English](../architecture.md) | 🇹🇷 Türkçe

# diyDaw Mimarisi

Bu belge diyDaw'ın iç yapısını anlatır: kodun hangi katmanlara ayrıldığını, hangi thread'in
neyi yaptığını, bir fare hareketinin hoparlöre kadar nasıl ulaştığını ve ses motorunun gerçek
zamanlı kurallarını. Kodla çalışacak herkes için yazılmıştır. Kısa ve İngilizce bir özet için
[AGENTS.md](../../AGENTS.md) dosyasına da bakın.

**İçindekiler**

1. [Genel bakış](#1-genel-bakış)
2. [Katmanlar ve klasörler](#2-katmanlar-ve-klasörler)
3. [Thread'ler ve gerçek zamanlı kurallar](#3-threadler-ve-gerçek-zamanlı-kurallar)
4. [Veri akışı: fare hareketinden hoparlöre](#4-veri-akışı-fare-hareketinden-hoparlöre)
5. [Ses motoru (engine/)](#5-ses-motoru-engine)
6. [Belge modeli (model/)](#6-belge-modeli-model)
7. [Eklenti barındırma](#7-eklenti-barındırma)
8. [Offline render (export)](#8-offline-render-export)
9. [Arayüz (app/)](#9-arayüz-app)
10. [Testler](#10-testler)
11. [Tasarım kararları ve gerekçeleri](#11-tasarım-kararları-ve-gerekçeleri)

---

## 1. Genel bakış

```
┌───────────────────────────────── app/ (JUCE GUI) ─────────────────────────────────┐
│  MainComponent ─ TransportBar, SongPositionBar, ArrangementView, Mixer, DetailView │
│                  ExportDialog, PluginManager, PluginWindows                        │
└───────────────┬───────────────────────────────────────────────────▲───────────────┘
                │ düzenleme komutları (geri alınabilir)              │ ValueTree okur,
                ▼                                                   │ dinleyicilerle yenilenir
┌───────────────────────────────── model/ (arayüzsüz) ──────────────────────────────┐
│  Project (ValueTree + UndoManager)   SamplePool   PluginHost   ProjectRenderer    │
└───────────────┬───────────────────────────────────────────────────────────────────┘
                │ kilitsiz anlık görüntüler (RealtimeSwap) ve atomik değerler
                ▼
┌───────────────────────────────── engine/ (gerçek zamanlı) ────────────────────────┐
│  Engine ─ Transport, AudioTrack ×16 (clip'ler, enstrüman, efektler, otomasyon),   │
│           master efektleri, master fader, Metronome                               │
└───────────────┬───────────────────────────────────────────────────────────────────┘
                │ Engine::render()
                ▼
        juce::AudioDeviceManager → ALSA / PipeWire / JACK → ses kartı
```

Üç katmanın görevleri kesin çizgilerle ayrılmıştır:

- **engine/** sadece ses üretir. Arayüzden de dosyalardan da habersizdir; ona ne çalacağı hazır,
  değişmez veri yapılarıyla verilir.
- **model/** projenin tek doğru kaynağıdır (single source of truth). Her düzenleme buradan geçer,
  geri alınabilir ve otomatik olarak motora iletilir.
- **app/** sadece gösterir ve komut verir. Motorun durumuna veya proje ağacına doğrudan yazmaz.

## 2. Katmanlar ve klasörler

```
diyDaw/
├── engine/            gerçek zamanlı ses motoru (JUCE: juce_audio_basics, juce_audio_formats)
│   ├── Engine         motorun tamamı; track havuzu, master, metronom, render döngüsü
│   ├── Transport      müzikal saat: beat cinsinden konum, tempo, döngü
│   ├── AudioTrack     bir kanal: clip'ler, enstrüman, efektler, otomasyon, fader/pan, ölçer
│   ├── Clip           ClipList, MidiSequence: track'e verilen değişmez anlık görüntüler
│   ├── Instrument     nota alan ses üreteci arayüzü (NoteEvent)
│   ├── Synth          dahili synth
│   ├── DrumRack       16 pad'li sampler
│   ├── BuiltinSamples kodla sentezlenen davul sesleri
│   ├── AudioEffect    efekt arayüzü (bypass geçişi, parametreler)
│   ├── Effects        EQ Three, Compressor, Reverb, Delay
│   ├── Automation     otomasyon zarfları (motor tarafı)
│   ├── Metronome      tık sesi
│   ├── SampleData     çözülmüş ses dosyası + dalga formu özeti
│   ├── PeakMeter      seviye ölçümü (ses thread'i → arayüz)
│   ├── RealtimeSwap   kilitsiz nesne aktarımı
│   └── ParamInfo      parametre tanımı (id, ad, aralık, varsayılan)
├── model/             arayüzsüz belge modeli
│   ├── Project        ValueTree belge, UndoManager, motor senkronizasyonu, dosya okuma/yazma
│   ├── SamplePool     ses dosyalarını arka planda çözen önbellek
│   ├── ProjectRenderer offline render (export)
│   ├── PluginHost     VST3/LV2 tarama ve yükleme
│   └── PluginDevices  eklentileri motorun efekt/enstrüman arayüzlerine saran sınıflar
├── app/               JUCE arayüz uygulaması
│   ├── arrangement/   zaman çizelgesi, cetvel, track başlıkları, clip çizimi, otomasyon şeritleri
│   ├── clipeditor/    piano roll
│   ├── devices/       synth paneli, drum rack paneli, cihaz zinciri
│   └── plugins/       eklenti yöneticisi, eklenti pencereleri
├── tests/             arayüzsüz birim testleri (+ tests/plugins: test eklentileri)
├── examples/          örnek projeler
└── external/JUCE      JUCE 9 (git submodule)
```

Bağımlılık yönü tek taraflıdır: `app → model → engine`. Motor hiçbir zaman modeli ya da arayüzü
bilmez; model arayüzü bilmez. (İstisna: eklenti barındırma için model `juce_audio_processors`
modülünü kullanır; bu modül arayüz koduna bağlıdır ama model arayüz bileşeni oluşturmaz.)

## 3. Thread'ler ve gerçek zamanlı kurallar

| Thread | Kim çalıştırır? | Ne yapar? |
| --- | --- | --- |
| **Mesaj thread'i** | JUCE | Arayüz, bütün model düzenlemeleri, dosya kaydetme/açma, eklenti oluşturma, eklenti taraması, çöp toplama |
| **Ses thread'i** | Ses sürücüsü | `Engine::render()` ve çağırdığı her şey |
| **Çözücü thread'leri** (2) | `SamplePool` | Ses dosyalarını arka planda okur |
| **MIDI giriş thread'i** | JUCE | MIDI klavye mesajlarını alır, canlı nota kuyruğuna yazar |
| **Export thread'i** | `ExportJob` | Projenin kopyasını offline render eder |

### Ses thread'inin kuralları

Ses kartı her birkaç milisaniyede bir yeni ses ister. Ses thread'i zamanında cevap veremezse
çıtırtı (dropout) duyulur. Bu yüzden `Engine::render()` içinde:

- **Bellek ayırma yok** (`new`, `std::vector::push_back`, `juce::String` oluşturma vb.). Her şey
  `prepare()` sırasında boyutlandırılır.
- **Kilit yok** (`std::mutex`, `CriticalSection`). Bekleme yok.
- **Dosya/konsol erişimi yok.**
- **İstisna (exception) yok.**

Thread'ler arası iletişim sadece üç yolla yapılır:

1. **`std::atomic`**: tekil değerler (fader, pan, mute, synth parametreleri, tempo, çalma durumu).
2. **`daw::RealtimeSwap<T>`**: büyük, değişmez nesnelerin el değiştirmesi (clip listesi, efekt
   zinciri, otomasyon, davul kiti, eklenti enstrümanı).
3. **`daw::PeakMeter`**: seviye ölçümleri (ses thread'i yazar, arayüz okuyup sıfırlar).

Tık sesine yol açabilecek parametre değişiklikleri (gain, pan, mute, efekt bypass'ı, filtre
cutoff'u ...) `juce::SmoothedValue` ile birkaç milisaniyeye yayılır.

### RealtimeSwap nasıl çalışır?

```
mesaj thread'i                          ses thread'i
───────────────                         ─────────────
publish(yeni)  ──► pending ──┐
                             │  acquire() blok başında:
                             └──► pending varsa active ile değiştir,
                                  eskisini "retired" kuyruğuna koy
collectGarbage() ◄── retired kuyruğu (SPSC, 64 yer)
   eski nesneyi siler
```

- Mesaj thread'i yeni nesneyi `publish()` ile bırakır. Ses thread'i henüz görmediyse önceki
  bekleyen nesne hemen silinebilir.
- Ses thread'i her blok başında `acquire()` çağırır ve o blok boyunca dönen işaretçiyi kullanır.
- Değiştirilen eski nesne silinmez; tek üreticili/tek tüketicili bir kuyruğa konur ve mesaj
  thread'i `collectGarbage()` ile (arayüz zamanlayıcısında, saniyede 30 kez) siler.

Böylece ses thread'i hiçbir zaman bellek ayırmaz ve serbest bırakmaz; `std::shared_ptr`
sayaçlarının sıfıra düşüp nesne silmesi de hep mesaj thread'inde olur.

## 4. Veri akışı: fare hareketinden hoparlöre

Örnek: kullanıcı arrangement'ta bir clip'i sürükleyip bırakıyor.

1. **Arayüz** (`ArrangementView`): sürükleme sırasında modele dokunmaz, sadece "hayalet" önizleme
   çizer. Fare bırakılınca:
   ```cpp
   project.beginTransaction ("Move clip");
   project.moveClip (clip, targetTrack, newStartBeat);
   ```
2. **Model** (`Project::moveClip`): ValueTree'yi `UndoManager` üzerinden değiştirir ve
   Ableton'ın üst üste binme kuralını uygular (`clearRegion`: alttaki clip'leri kırpar, böler,
   siler). Bütün bu değişiklikler aynı transaction'dadır; tek bir Ctrl+Z hepsini geri alır.
3. **Model dinleyicisi** (`Project::valueTreePropertyChanged` / `ChildAdded` / `ChildRemoved`):
   etkilenen track için `syncTrack()` çağrılır. Bu, track'in bütün clip'lerinden yeni bir
   değişmez `ClipList` oluşturur (başlangıca göre sıralı; MIDI clip'lerin notaları
   `MidiSequence`'e kopyalanır; ses clip'lerinin `SampleData`'sı `shared_ptr` ile paylaşılır).
4. **Motor** (`AudioTrack::setClips` → `RealtimeSwap::publish`): yeni liste bir sonraki ses
   bloğunun başında ses thread'ine geçer.
5. **Arayüz** de aynı ValueTree'yi dinlediği için yeniden çizilir.

Geri alma (undo) da aynı yolu izler: `UndoManager` ValueTree'yi eski haline getirir, dinleyici
motoru günceller, arayüz yeniden çizilir. Bu yüzden "geri al" için ayrı bir motor kodu yoktur.

Kural olarak:

- Arayüz **asla** ValueTree'ye doğrudan yazmaz ve **asla** motorun setter'larını (örneğin
  `AudioTrack::setGainDb`) çağırmaz. Her şey `Project`'in metotlarından geçer.
- Her kullanıcı hareketi **tek** bir `beginTransaction()` ile başlar. Sürgüler ve düğmeler için
  `app/ProjectControls.h` içindeki `bindSliderToProject` / `bindToggleToProject` bunu otomatik
  yapar: sürükleme başında transaction açılır, sürükleme boyunca aynı özelliğe yapılan ardışık
  değişiklikler `UndoManager` tarafından tek adıma birleştirilir.

## 5. Ses motoru (engine/)

### Engine

`Engine` bütün ses motorudur ve ses kartından bağımsızdır: uygulama onu ses kartının
callback'inden, testler ve export ise doğrudan çağırır.

- **Track havuzu:** 16 `AudioTrack` önceden oluşturulur. Track eklemek bir sayacı artırmaktır;
  ses thread'inde bellek ayırmaz.
- **`prepare(sampleRate, maxBlockSize)`:** her şeyi boyutlandırır (ses kartı açılırken, ses
  durmuşken çağrılır). Efektleri ve eklentileri de yeniden hazırlar.
- **`render(out, numSamples)`:** ses kartının istediği bloğu en fazla **256 sample**'lık
  parçalara böler (`automationBlockSize`; otomasyon bu sıklıkta güncellenir). Her parça için:

```
Transport::advance(n)  →  Block { playing, startBeat, beatsPerSample, bpm, sampleRate, numSamples }
       │                    (döngü sonuna denk gelirse blok kısalır, konum döngü başına sarar)
       ▼
for each track (ya da pattern modunda sadece izole track):
    AudioTrack::process(bus, block, anySolo)   ── bus'a toplar
       ▼
master efektleri  →  master gain (yumuşatılmış)  →  metronom  →  master ölçer  →  çıkış
```

### Transport

- Konum **beat** cinsinden `double` olarak tutulur; tempo değişince müzikal konum korunur.
- Her blok bir öncekinin **tam bittiği yerden** başlar (`positionBeats = block.endBeat()`), bu
  yüzden `[başlangıç, bitiş)` aralık kontrolleri hiçbir vuruşu kaçırmaz veya iki kez saymaz.
- Arayüzden gelen "şuraya atla" istekleri (`setPositionBeats`) bir sonraki blok başında uygulanır.
- **Döngü:** `setLoop(start, end)` + `setLoopEnabled(true)`. Bir blok döngü sonunu geçecekse
  `advance()` bloğu tam döngü sonunda keser ve konumu döngü başına alır; `Engine::render`
  kalan kısım için tekrar `advance()` çağırır. Böylece döngü sample hassasiyetindedir.

Beat → sample dönüşümünde, kayan nokta hatalarını yutmak için 1/1000 sample'lık bir tolerans
kullanılır: bir olay `ceil((beat - blockStart) / beatsPerSample - 0.001)` sample'ına düşer. Aynı
kural clip kenarlarında, MIDI notalarında ve metronomda kullanılır; bu sayede çıktı blok
boyutundan bağımsızdır (testlerde 1, 7, 64, 512 ve 4096 sample'lık bloklarla doğrulanır).

### AudioTrack

Bir track'in her bloktaki işlem sırası:

1. **Otomasyon:** her zarfın playhead'deki değeri ilgili parametreye yazılır (çalma durmuşken de).
2. **Ses clip'leri:** bloğa denk gelen her clip'in sesi, doğrusal interpolasyonla (sample rate
   farkı için) karalama tamponuna (scratch) eklenir. Clip kenarlarında 2 ms yumuşatma vardır.
   Loop'lu clip'lerde sample başa sarar.
3. **Enstrüman** (MIDI track'lerde): MIDI clip'lerden ve canlı nota kuyruğundan bloğun
   `NoteEvent` listesi çıkarılır ve enstrüman işletilir (aşağıya bakın).
4. **Efekt zinciri:** sırayla, yerinde (in-place) işlenir.
5. **Fader, pan, mute/solo:** sabit-güç (constant-power) pan yasası; değerler `SmoothedValue`
   ile yumuşatılır.
6. **Ölçer** ve **toplama:** sonuç master bus'a eklenir.

#### MIDI zamanlaması

`AudioTrack::renderInstrument` her blokta:

- Bloğa düşen nota başlangıçlarını bulur. MIDI clip'in içerik zamanı
  `offsetBeats + (t - clipStart)` olarak hesaplanır; loop'lu clip'lerde içerik `loopLength`'e göre
  sarar ve loop sonunu aşan notalar orada kesilir.
- **Çalan notaları kendisi takip eder:** her perde için "bu nota ne zaman bitecek" bilgisini
  saklar. Böylece clip silinse, nota değiştirilse, çalma dursa ya da playhead atlasa bile hiçbir
  nota asılı kalmaz (durma/atlama anında bütün sıralı notalar kapatılır).
- Aynı perde kısa aralıklarla tekrar çalınırsa önceki notanın kapanışı kendi bitiş noktasına
  yerleştirilir ve hiçbir zaman kendi açılışından önceye düşmez.
- **Canlı notalar** (bilgisayar klavyesi, MIDI klavye, piano roll dinleme) `pushLiveNote()` ile
  kilitsiz bir FIFO'ya yazılır (üreticiler kendi aralarında bir mutex kullanır; ses thread'i
  kullanmaz) ve bloğun başında **geliş sırasıyla** eklenir. Sıralı (sequencer) olaylar ise
  sample konumuna göre, aynı konumda önce kapanışlar olacak şekilde sıralanır; bu sıralama
  bellek ayırmayan bir insertion sort ile yapılır.

Track'in enstrümanı `InstrumentType` ile seçilir: `none`, `synth`, `drumRack` ya da `plugin`.
Synth ve drum rack her track'te hazır bekler; eklenti enstrümanı ayrı bir `RealtimeSwap` yuvası
üzerinden gelir. Tür değişince eski enstrüman ses thread'inde sıfırlanır.

### Synth

16 sesli subtractive synth (`engine/Synth`):

- Osilatörler: PolyBLEP ile band-limited saw ve square, PolyBLAMP ile triangle, sine.
- Filtre: TPT (topology-preserving transform) yapısında rezonanslı alçak geçiren filtre; cutoff,
  envelope miktarı ve key tracking ile her 16 sample'da bir hesaplanır.
- Envelope'lar: doğrusal segmentli ADSR; attack ve release o anki seviyeden başlar (tık yok).
- Ses çalma (voice stealing): aynı perdeyi çalan ses, yoksa boş ses, yoksa en eski bırakılmış
  ses, yoksa en eski ses; çalınan ses 4 ms'de söndürülür.
- Cutoff, volume ve mix yumuşatılır. Parametreler atomiktir ve `ParamInfo` listesiyle tanımlanır.

### DrumRack ve hazır sesler

- 16 pad, notalar 36–51. Tek vuruşluk (one-shot) çalma, 32 ses, choke grupları.
- Pad başına gain, pan, tune (yeniden örnekleme), decay ve choke parametreleri.
- Davul kiti (`DrumRack::Kit`, 16 `SampleData`) `RealtimeSwap` ile gelir. Kit değiştiğinde, çalan
  bir sesin sample'ı yeni kitte yoksa ses eski belleğe hiç dokunmadan söner: her ses, sample'ın
  sonraki 5 ms'sini önceden kopyalanmış olarak tutar (lookahead tamponu).
- `BuiltinSamples`: kick, snare, hi-hat'ler, tom'lar vb. kodla sentezlenir (sabit tohumlu gürültü,
  deterministik). `SamplePool::get("builtin:kick")` ilk istendiğinde sesi üretir.

### Efektler

`AudioEffect` temel sınıfı:

- Parametreleri atomik bir dizide tutar (`setParam` aralığa sıkıştırır, adımlı parametreleri
  yuvarlar).
- `process()` bypass'ı yönetir: efekt kapatılıp açılırken kuru ve işlenmiş ses arasında 10 ms'lik
  geçiş yapılır; tamamen kapalıyken efekt hiç işletilmez. Uzun süre kapalı kalıp tekrar açılan
  efektin eski kuyruğu (örneğin reverb) temizlenir.
- `prepare()` hazırlanan sample rate'i kaydeder; ses kartı değişirken farklı bir hızda gelen
  blokta efekt atlanır (güvenlik).

Efekt zinciri (`EffectChain`) `shared_ptr` listesidir. Model her efekt DEVICE'ı için **tek bir
motor örneği** tutar; zincir yeniden oluşturulduğunda (efekt ekleme, silme, sıralama) aynı
örnekler yeni listeye konur, böylece bir reverb'ün kuyruğu başka bir efekt eklenince kesilmez.

Dahili efektler: EQ Three (RBJ biquad'lar, 32 sample'lık alt bloklarda yumuşatılmış katsayılar),
Compressor (stereo bağlı, dB alanında tepe dedektörü, karesel knee), Reverb (Freeverb tarzı, sample
rate'e ölçeklenmiş, pre-delay'li), Delay (kesirli gecikme, filtrelenmiş geri besleme, ping-pong,
yumuşak süre değişimi).

### Otomasyon (motor tarafı)

Her track'in `AutomationList`'i bir `RealtimeSwap` ile gelir. Her `AutomationLane` bir hedefe
(track gain, track pan, enstrüman parametresi ya da belirli bir efekt örneğinin parametresi)
ve sıralı noktalara sahiptir. `AudioTrack::applyAutomation`, bloğun başlangıç beat'indeki değeri
ikili aramayla bulur ve hedefe yazar. Değer modelde tutulan değerin üzerine yazılır; zarf
kaldırılınca model parametrenin kendi değerini motora tekrar gönderir.

## 6. Belge modeli (model/)

### Project

`Project` bir `juce::ValueTree` ve bir `juce::UndoManager` tutar. Ağaç yapısı (ayrıntılar için
[dosya-formati.md](dosya-formati.md)):

```
PROJECT { version, bpm, beatsPerBar, masterGain }
├── TRACK { kind, name, colour, engineIndex, gain, pan, mute, solo, height }
│   ├── CLIP (audio) { file, start, length, offset, loop }
│   ├── CLIP (midi)  { start, length, offsetBeats, loop, loopLength }
│   │   └── NOTE { pitch, start, length, velocity }
│   ├── DEVICE (enstrüman: synth | drumrack | pluginInstrument)
│   ├── DEVICE (efekt: eq | compressor | reverb | delay | plugin)  ...
│   └── AUTOMATION { target }
│       └── POINT { beat, value }
└── MASTER
    └── DEVICE (efekt) ...
```

Önemli metot grupları:

| Grup | Örnekler |
| --- | --- |
| Belge | `newDocument`, `saveTo`, `loadFrom`, `copyFrom`, `createSnapshot`, `hasUnsavedChanges` |
| Track | `addTrack`, `setTrackName`, `setTrackGain/Pan/Mute/Solo`, `setTrackHeight`, `getNumTracks`, `getTrack` |
| Clip | `addClip`, `addMidiClip`, `moveClip`, `trimClipStart/End`, `splitClip`, `duplicateClip`, `deleteClip`, `setClipLooping`, `setClipLoopLength` |
| Nota | `addNote`, `moveNote`, `setNoteLength`, `setNoteVelocity`, `deleteNote` |
| Cihaz | `setDeviceParam`, `addEffect`, `removeEffect`, `moveEffect`, `setEffectEnabled`, `setInstrument`, `setPadSample` |
| Eklenti | `setPluginHost`, `addPluginEffect`, `setPluginInstrument`, `getPluginInstance`, `getPluginError` |
| Otomasyon | `getAutomationTargets`, `getOrCreateAutomationLane`, `addAutomationPoint`, `moveAutomationPoint`, `deleteAutomationPoint`, `removeAutomationLane` |

### Senkronizasyon

`Project` kendi ağacının dinleyicisidir. Hangi düğümün değiştiğine göre motorun ilgili kısmını
günceller:

| Değişiklik | Çağrılan | Motora giden |
| --- | --- | --- |
| CLIP / NOTE | `syncTrack` | yeni `ClipList` |
| TRACK gain/pan/mute/solo | `syncTrackMixer` | atomik değerler |
| Enstrüman DEVICE eklendi/çıkarıldı | `syncDevice` | enstrüman türü, parametreler, eklenti yuvası, davul kiti |
| Efekt DEVICE eklendi/çıkarıldı/sırası değişti | `syncEffects` | yeni `EffectChain` |
| DEVICE parametresi | (doğrudan) | ilgili örneğin `setParam`'ı |
| `pad<N>File` | `syncDrumKit` | yeni `DrumRack::Kit` |
| AUTOMATION / POINT | `syncAutomation` | yeni `AutomationList` + otomasyonu kalkan parametrelerin model değerleri |
| PROJECT bpm / beatsPerBar / masterGain | (doğrudan) | Transport / Engine |

Track indeksleri ile motor track'leri arasındaki bağ `engineIndex` özelliğidir (çalışma zamanı
bilgisidir, dosyaya yazılmaz). Proje açılınca motorun track havuzu sıfırlanır ve track'ler sırayla
yeniden bağlanır.

Proje açarken (`replaceContents`) dosyadaki veri temizlenir: XML'den gelen dizeler sayıya ve
mantıksal değere çevrilir, aralık dışı değerler sıkıştırılır, track türüne uymayan clip'ler ve
bilinmeyen cihazlar atılır, eksik synth eklenir, cihaz `uid`'leri tekilleştirilir. Böylece kodun
geri kalanı ağacın yapısına güvenebilir.

### SamplePool

- Ses dosyalarını tam yollarına göre önbelleğe alır.
- `loadAsync()` dosyayı 2 thread'lik bir havuzda çözer, sonucu mesaj thread'ine geri gönderir.
  Aynı dosya için gelen istekler tek bir çözümlemeye birleştirilir; geri çağırma her zaman
  asenkrondur. Havuz silinirse bekleyen işler bir `weak_ptr` sayesinde güvenle yok sayılır.
- Yeni bir sample geldiğinde `onSampleAdded` ile proje bütün track'leri yeniden senkronlar
  (yüklenmeyi bekleyen clip'ler çalmaya başlar).
- Her `SampleData` için 256 sample'lık bloklardan bir min/max dalga formu özeti hesaplanır;
  arayüz dalga formunu bundan çizer.

### Geri alma ayrıntıları

- `beginTransaction()` her kullanıcı hareketinin başında bir kez çağrılır.
- Geri alınabilir olmayan değişiklikler (`nullptr` UndoManager ile yapılanlar): track ekleme, track
  yüksekliği, `engineIndex`, `uid` onarımı, eklenti durumunun silinmeden önce yakalanması.
- Eklentiler: bir eklenti DEVICE'ı silinmeden hemen önce eklentinin o anki durumu DEVICE'ın
  `state` özelliğine yazılır; geri alma DEVICE'ı geri getirince yeni eklenti örneği bu durumla
  açılır.

## 7. Eklenti barındırma

```
PluginHost (model/)                            PluginDevices (model/)
├── AudioPluginFormatManager                   ├── PluginEffect : AudioEffect
│     uygulama: editörlü formatlar             │     processBlock(stereo tampon, boş MIDI)
│     testler:  headless formatlar             └── PluginInstrument : Instrument
├── KnownPluginList (katalog)                        NoteEvent → MidiBuffer (önceden ayrılmış)
├── tarama: mesaj thread'inde, zamanlayıcıyla dosya dosya,
│           dead-man's-pedal dosyasıyla çöken eklentiyi kara listeye alma
└── createInstance (senkron)
```

- Eklenti DEVICE'ı `description` (JUCE `PluginDescription` XML'i) ve `state` (eklentinin kendi
  durumu, base64) saklar.
- Model, efekt eklentisini `syncEffects` içinde, enstrüman eklentisini `syncDevice` içinde oluşturur,
  kayıtlı durumu yükler ve motorun hazırlanmış sample rate/blok boyutuyla `prepareToPlay` çağırır.
- Durum, eklentinin içinde yaşar; `Project::createSnapshot()` kaydetme ve export sırasında bütün
  eklentilerin durumunu ağacın **kopyasına** yazar (canlı ağaca yazmak geri alma geçmişini
  kirletirdi).
- Eklentinin kendi arayüzünde parametre değişince bir `AudioProcessorListener` projenin
  "kaydedilmemiş değişiklik" bayrağını (atomik) kaldırır.
- Eklenti yüklenemezse DEVICE ağaçta kalır, motorda yer almaz ve hata mesajı
  `getPluginError()` ile okunabilir.
- Uygulama kapanırken eklenti örnekleri, onları yükleyen `PluginHost`'tan önce yok edilir
  (`MainComponent` üye sırası bunu garanti eder).
- Bilinen JUCE davranışları: bazı formatlar `prepareToPlay`'i "değişiklik yok" diye atlayabildiği
  için yeniden hazırlamadan önce `releaseResources()` çağrılır; LV2 parametre değişikliklerini bir
  sonraki `processBlock`'ta uygular.

## 8. Offline render (export)

`ProjectRenderer`:

1. Mesaj thread'inde kendi `Engine`'ini ve kendi `Project`'ini oluşturur, kaynak projenin
   `createSnapshot()` kopyasını yükler (eklentiler kayıtlı durumlarıyla yeniden oluşturulur, sample'lar
   aynı `SamplePool`'dan paylaşılır).
2. Bundan sonra paylaşılan hiçbir şeye dokunmadığı için render arka plandaki export thread'inde
   yapılabilir; canlı motor çalmaya devam eder.
3. Başlamadan önce motoru durdurulmuş halde 50 ms çalıştırır (fader yumuşatmaları hedefe otursun
   diye; aksi halde export bir "fade-in" ile başlardı).
4. Aralığı render eder; aralık sonunda transport'u durdurur ve **efekt kuyruğunu** (sabit süre ya
   da sessizliğe kadar) render etmeye devam eder.
5. Normalize seçiliyse iki geçiş yapar: önce tepe değeri bulur, sonra kazançla yazar. 16 bit'te
   TPDF dither ekler. Dosyayı önce geçici bir dosyaya yazar, başarılı olunca hedefin yerine koyar.

## 9. Arayüz (app/)

| Bileşen | Görev |
| --- | --- |
| `MainComponent` | Ses cihazı, motor, proje, eklenti sunucusu; dosya işlemleri; kısayollar; canlı nota yönlendirme; pattern modu; 30 Hz zamanlayıcı (ölçüler, playhead, çöp toplama) |
| `TransportBar` | Tempo, metronom, PAT/SONG, çal/dur, konum, track ekleme |
| `SongPositionBar` | Şarkı konum çubuğu |
| `ArrangementView` | Zaman çizelgesi: track şeritleri, clip'ler, sürükleme önizlemeleri, dosya bırakma, otomasyon şeritleri, değişken yükseklikli düzen |
| `TrackHeader`, `TimeRuler`, `ClipPainter`, `TimelineGeometry`, `AutomationLane` | Arrangement parçaları |
| `DetailView` | Alt panel: Clip ve Device sekmeleri, Master düğmesi |
| `PianoRoll` | MIDI nota editörü |
| `DeviceChain`, `SynthPanel`, `DrumRackPanel` | Cihaz zinciri ve paneller |
| `TrackStrip`, `MasterStrip`, `LevelMeter` | Mikser |
| `ExportDialog` | Export penceresi ve arka plan işi |
| `PluginManager`, `PluginWindows` | Eklenti yöneticisi ve editör pencereleri |
| `DawLookAndFeel` | Koyu tema, düğme/sürgü çizimleri, otomasyon rengi |

Arayüz bileşenleri modeli iki yolla takip eder: ağaç yapısı değişikliklerini (track/cihaz eklendi,
çıkarıldı) `ValueTree::Listener` ile dinler; değerleri (fader konumları, düğmeler) ise 30 Hz
zamanlayıcıyla modelden okur ve sürüklenmekte olan kontrolün değerini ezmez. Otomasyonlu
parametrelerde modelin değeri yerine motorun o anki değeri gösterilir.

Bir bileşenin kendi geri çağırması sırasında silinmemesi için bazı yeniden kurmalar
`MessageManager::callAsync` + `SafePointer` ile ertelenir (örneğin bir efektin "x" düğmesi kendi
panelini siler).

## 10. Testler

- `tests/` altındaki `juce::UnitTest` sınıfları "diyDaw" kategorisindedir ve `DawEngineTests`
  konsol programıyla çalışır. Program bir `ScopedJuceInitialiser_GUI` açar (eklentiler bir
  `MessageManager` ister).
- Testler arayüzsüzdür: motoru doğrudan `render()` / `renderOffline()` ile çalıştırır, çıktıyı
  sample sample karşılaştırır. Ses dosyası yerine bellekte üretilen "sanal" sample'lar kullanılır.
- Kapsam (yaklaşık 430 test bölümü): transport, metronom, clip çalma, proje düzenleme kuralları,
  kaydetme/açma, export, synth, MIDI zamanlaması, canlı notalar, efektler, efekt zinciri ve döngü,
  otomasyon, master ve efekt kuyruğu, drum rack, drum track'leri, eklentiler (VST3 ve LV2).
- `tests/plugins` iki küçük test eklentisi derler; eklenti testleri bunları `DAW_TEST_PLUGIN_DIR`
  üzerinden tarar.
- Bir ilke: motor ya da modelde davranış değiştiren her değişiklik için `tests/`'e test eklenir.

## 11. Tasarım kararları ve gerekçeleri

| Karar | Neden? |
| --- | --- |
| Konum beat cinsinden `double` | Tempo değişince müzikal konum korunur; MIDI ve ızgara doğal olarak beat cinsindendir |
| Her blok bir öncekinin bitişinden başlar | Yuvarlama hataları birikmez; olaylar kaçmaz ya da iki kez tetiklenmez |
| Track başına değişmez `ClipList` anlık görüntüsü | Ses thread'i hiç kilit almadan tutarlı bir liste okur; düzenleme ne kadar karmaşık olursa olsun motor tek bir işaretçi değiştirir |
| 16 track'lik sabit havuz | Track eklemek ses thread'inde bellek ayırmaz |
| ValueTree + UndoManager | Kaydetme (XML), geri alma ve değişiklik bildirimleri tek bir yapıdan gelir |
| Sürükleme sırasında önizleme, bırakınca tek düzenleme | Üst üste binme kuralı sürükleme sırasında diğer clip'leri kesip durmaz; tek geri alma adımı |
| Efekt DEVICE'ı başına tek motor örneği | Zincir değişse de efektin iç durumu (kuyruk, envelope) korunur |
| Export ayrı motor ve proje kopyasıyla | Export canlı çalmayı etkilemez, arka planda güvenle yapılır |
| Hazır davul sesleri kodla sentezlenir | Lisans sorunu yok, depo ve proje dosyaları küçük kalır |
| Testlerde kendi derlediğimiz eklentiler | Testler sistemde hangi eklentilerin kurulu olduğuna bağlı değildir |
