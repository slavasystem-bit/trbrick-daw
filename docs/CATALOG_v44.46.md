# Каталог патернів і пресетів (v44.46)

> Оновлення v44.47: 13 патернів ударних тепер довгі (AMEN 4 такти; JUNGLE, BREAKBEAT, BREAKS, DRUM AND BASS, UK GARAGE, DUBSTEP, TRAP, HIP HOP, BOOM BAP, LO-FI HIP HOP, FUNK, AFROBEAT по 2). Решта цифр нижче без змін.

> Оновлення v44.49: тепер є поле «стиль» (`core/Styles.h`): 12 пакетів (HOUSE, DEEP HOUSE, TECH HOUSE, UK GARAGE, TECHNO, ACID TECHNO, ELECTRO, BREAKS, JUNGLE, DRUM AND BASS, BOOM BAP, LO-FI HIP HOP), кожен посилається на наявний патерн, кіт, групу, чотири пресети й прогресію та має басову лінію й BPM. Решта цього каталогу описує стан до цього.

Список знято з коду (`DrumPatterns.cpp`, `DrumBreaks.cpp`, `Grooves.h`, `ChordProgressions.cpp`, `SynthPresets.cpp`, `KitInfo.h`, `Risers.cpp`, `Types.cpp`) окремою програмою, яка друкує справжні масиви, а не з пам'яті.

**Важливо про «стилі».** У програмі немає поля «стиль»: патерн має лише назву й свінг, пресет синтезатора має категорію інструмента (BASS, LEAD ...), кіт має категорію (CLASSIC, ELECTRONIC ...), прогресія має лише ступені. Розкладка по стилях нижче це моє зіставлення за назвами й загальноприйнятими конвенціями; у коді його немає. Це само по собі висновок (розділ 4).

## 1. Підсумок у цифрах

| Що | Скільки | Чим розрізняється в програмі |
|---|---|---|
| Патерни ударних (GenrePattern) | 27 | ім'я + свінг; лише барабани, 16 кроків, повторюється на довшу довжину |
| Живі брейки / філи (DrumBreak) | 16 | за функцією (ролл, пауза, стоп), не за стилем; кожен має корекцію гучності в дБ |
| Групи (Groove) | 11 | 16 кроків {зсув %, гучність}; плюс власні в `grooves/` |
| Прогресії акордів | 19 | лише ступені 1-7 (працюють у будь-якому ладі); без стилю |
| Пресети синтезатора | 56 | категорія: BASS 17, LEAD 10, PAD 9, PLUCK 9, KEYS 7, FX 4; рушій: 42 субтрактивних, 9 FM, 5 хвильових таблиць |
| Кіти ударних | 19 | категорія: CLASSIC 7, ELECTRONIC 5, GROOVE 5, HARD 2; плюс теги (SUB, PUNCHY ...) |
| Райзери й переходи | 14 | 10 файлів riser0-9 + 4 синтезованих (DOWNLIFTER, IMPACT, SUB DROP, REVERSE CRASH) |
| Хвильові таблиці | 4 | CLASSIC, HARMONIC SWEEP, FORMANT, PULSE WIDTH |
| Майстер-ланцюг | 4 | CLEAN, WARM, LOUD, LO-FI |
| Стартовий груув | 1 | 72 BPM, свінг 10 %, кік / снейр / клеп / хет / перк + акорд |
| Ролі сцен | 5 | INTRO, BUILD, DROP, BREAK, OUTRO |
| Власне (на диску) | — | My Beats, власні пресети синтезатора (`Library/Presets/*.t8syn`, категорія MY), власні групи (`.t8groove`); їх кількість залежить від користувача |

## 2. Розкладка по стилях, за повнотою (від найповнішого)

Повнота = скільки є готового саме для стилю: патерни, кіти, групи, басові пресети, акордові / лідові пресети. Прогресії в таблицю не входять (вони не прив'язані до стилів), їх зіставлено в розділі 3.

| № | Стиль | Патерни ударних | Кіти | Групи | Бас | Стаби, ліди, клавіші, пади |
|---|---|---|---|---|---|---|
| 1 | **Техно і сусіди** | 6: TECHNO, MINIMAL TECHNO, ACID TECHNO, DUB TECHNO, INDUSTRIAL TECHNO, EBM | techno, minimal, industrial, acid, acid_v2 (+ gabber) | TECHNO DRIVE, ACID BOUNCE | TECHNO BASS, ACID BASS, ACID SQUARE, ACID SQUEAL, SUB BASS | TECHNO STAB, RAVE STAB, ACID LEAD, HOOVER, DUB CHORD, DARK PAD, AMBIENT DRONE |
| 2 | **Хаус, диско, гараж** | 6: FOUR ON THE FLOOR, HOUSE, DEEP HOUSE, TECH HOUSE, UK GARAGE, DISCO | 909, 707, 606 | FOUR FLOOR ACCENT, LAID BACK | HOUSE BASS, SUB+SAW, FAT BASS | HOUSE STAB, CHORD STAB, FM EPIANO, ELECTRIC ORGAN, CLAV, TINE KEYS, WARM PAD, ANALOG PAD |
| 3 | **Брейки, джангл, драм-н-бейс, дабстеп** | 6: BREAKS, BREAKBEAT, AMEN, JUNGLE, DRUM AND BASS, DUBSTEP | breaks, dub | BREAKS SHUFFLE | REESE BASS, WOBBLE BASS, FM GROWL, DUB BASS, WT VOWEL BASS | SUPER SAW, WT GLASS LEAD, брейки-філи (BREAK CHOP, DOUBLE TIME, STUTTER KICK, GHOST SNARE FUNK) |
| 4 | **Хіп-хоп, трап, лоу-фай** | 4: HIP HOP, BOOM BAP, LO-FI HIP HOP, TRAP | lofi, trap808, 808, linn | MPC 16TH 58, MPC 16TH 66 TRIPLET, LAID BACK, DRUNK | SUB 808, SUB BASS | FM EPIANO, TINE KEYS, CLAV, MARIMBA, GLASS BELLS, SOFT PLUCK |
| 5 | **Фанк і світові (афро, реггетон)** | 3: FUNK, AFROBEAT, REGGAETON | funk, cr78 | HUMAN TIGHT, DRUNK | RUBBER BASS | CLAV, BRASS STAB, ELECTRIC ORGAN, MARIMBA |
| 6 | **Електро** | 2: ELECTRO, ELECTRO FUNK | electro, cr78, 808 | ELECTRO PUSH | ELECTRO BASS, RUBBER BASS, FM BASS | MONO SAW LEAD, SQUARE LEAD, ARP PLUCK |
| 7 | **Транс і рейв, хард** | 0 | gabber (єдиний кіт без патерна) | — | — | TRANCE GATE, SUPER SAW, HOOVER, RAVE STAB |
| 8 | **Ембієнт, кіно** | 0 | — | — | — | AMBIENT DRONE, WIDE PAD, GLASS PAD, CHOIR PAD, WT SWEEP PAD, STRING PAD, WIND |
| 9 | **Поп, рок, співак-автор** | 0 | — | — | — | лише прогресії (розділ 3) |

Сума патернів: 6 + 6 + 6 + 4 + 3 + 2 = 27, тобто кожен із 27 стоїть у рівно одному рядку.

### 2.1 Патерни ударних за назвою (27)

Свінг у дужках, якщо не нуль.

| Родина | Патерни |
|---|---|
| Хаус | FOUR ON THE FLOOR, HOUSE (0.10), DEEP HOUSE (0.12), TECH HOUSE (0.06), UK GARAGE (0.20), DISCO |
| Техно | TECHNO, MINIMAL TECHNO, ACID TECHNO, DUB TECHNO, INDUSTRIAL TECHNO, EBM |
| Електро | ELECTRO, ELECTRO FUNK |
| Брейки і бас-музика | BREAKS (0.08), BREAKBEAT (0.10), AMEN, JUNGLE, DRUM AND BASS, DUBSTEP |
| Хіп-хоп | HIP HOP (0.18), BOOM BAP (0.14), LO-FI HIP HOP (0.22), TRAP |
| Фанк і світові | FUNK (0.08), AFROBEAT, REGGAETON |

AMEN, AFROBEAT і REGGAETON в коді прямо названо «у стилі»: їхні справжні ритми складніші, ніж один 16-кроковий такт.

### 2.2 Живі брейки (16), за призначенням

| Призначення | Брейки |
|---|---|
| Ролли | SNARE ROLL, HAT ROLL, KICK ROLL, CLAP ROLL, MILITARY ROLL, TOM FILL |
| Ритмічні філи | GHOST SNARE FUNK, BREAK CHOP, LATIN FLURRY, DOUBLE TIME, STUTTER KICK, ACID ROLL |
| Напруга й пауза | RISER BUILD, REVERSE SWELL, TENSION STOP, HALF-TIME DROP |

### 2.3 Пресети синтезатора за категорією (56)

| Категорія | Пресети |
|---|---|
| BASS (17) | ACID BASS, ACID SQUARE, ACID SQUEAL, SUB BASS, SUB 808, FAT BASS, SUB+SAW, REESE BASS, WOBBLE BASS, DUB BASS, TECHNO BASS, HOUSE BASS, RUBBER BASS, ELECTRO BASS, FM BASS, FM GROWL, WT VOWEL BASS |
| LEAD (10) | SUPER SAW, DETUNED LEAD, MONO SAW LEAD, SQUARE LEAD, SOFT LEAD, ACID LEAD, HOOVER, TRANCE GATE, WT PWM LEAD, WT GLASS LEAD |
| PAD (9) | WARM PAD, WIDE PAD, STRING PAD, DARK PAD, AMBIENT DRONE, ANALOG PAD, WT SWEEP PAD, CHOIR PAD, GLASS PAD |
| KEYS (7) | FM EPIANO, FM BELL, TINE KEYS, ELECTRIC ORGAN, CLAV, MARIMBA, GLASS BELLS |
| PLUCK (9) | TECHNO STAB, CHORD STAB, HOUSE STAB, RAVE STAB, BRASS STAB, PLUCK, SOFT PLUCK, ARP PLUCK, DUB CHORD |
| FX (4) | NOISE SWEEP, WIND, LASER ZAP, SIREN |

### 2.4 Кіти (19) і групи (11)

| Категорія кітів | Кіти |
|---|---|
| CLASSIC (7) | 606, 707, 808, 808perc, 909, cr78, linn |
| ELECTRONIC (5) | acid, acid_v2, electro, techno, minimal |
| GROOVE (5) | breaks, dub, funk, lofi, trap808 |
| HARD (2) | gabber, industrial |

Групи: ELECTRO PUSH, TECHNO DRIVE, BREAKS SHUFFLE, ACID BOUNCE (за стилем); MPC 16TH 58, MPC 16TH 66 TRIPLET (сімейство MPC); LAID BACK, PUSHING, DRUNK, HUMAN TIGHT, FOUR FLOOR ACCENT (за характером).

## 3. Прогресії акордів (19): за функцією, не за стилем

У коді вони не мають стилю, лише коментарі. Нижче те, що там написано, без моїх домислів про жанри.

| Група | Прогресії | Що кажуть коментарі в коді |
|---|---|---|
| Основні | 1-4-5-1, 1-5-6-4, 6-4-1-5, 1-6-4-5, 1-6-2-5, 2-5-1, 1-4-1-5, 1-5-4-5, 4-5-1-1, 1-4-6-5, 6-5-4-5, 1-7-6-7, 1-3-4-5, 6-2-5-1, 2-4-1-5, 1-2-4-1 | 1-5-6-4 «найвживаніша поп-прогресія», 1-6-4-5 «п'ятдесяті», 1-6-2-5 і 2-5-1 «джазові», 1-7-6-7 «модальна, рок», 6-5-4-5 «андалузька», 6-2-5-1 «коло квінт» |
| Довга форма | 1-5-6-3-4-1-4-5 | «канон», 8 акордів |
| Вампи (для арпеджіо) | 1-4 VAMP, 1-5 VAMP | двохакордові |

## 4. Що з цього випливає

1. **Усі «типові патерни» це лише барабани.** Для баса, акордів і мелодії готових стильових шаблонів немає; є лише випадковий генератор у ладі й прогресії. Стиль зараз задає тільки ударні.
2. **Стиль не навігується.** Патерни, кіти, пресети, групи лежать плоскими списками у різних місцях (RANDOM / KIT, SOUND, CHORD LAB, групи). Щоб зібрати «хаус», треба окремо знайти патерн, кіт, групу, басовий пресет і прогресію.
3. **Нерівномірність.** Техно, хаус і брейки по 6 патернів, електро 2, фанк і світові 3, а транс, ембієнт, поп і хард (хоч є кіт gabber) не мають жодного патерна.
4. **Які стилі зовсім відсутні** (за переліком вище): транс, хардстайл і габер-патерн, ембієнт, реггей і даб (даб є лише в техно), драм-н-бейс окремими варіаціями (ліквід, нейро), дрилл, афро-хаус, синтвейв і італо, будь-що в 6/8 чи тріолях (тріоль є лише як група MPC), поп і рок. Це не перелік того, що «треба», а те, чого немає.
5. **Повтори, які варто порівняти** (не перевірено на слух): три кислотні басові пресети; BREAKS, BREAKBEAT, AMEN і JUNGLE як сусіди; ACID TECHNO і TECHNO.

## 5. Як це можна впорядкувати (пропозиція, у коді поки нічого не змінено)

Додати одне поле `style` до патернів, пресетів, кітів, груп і прогресій (набір: HOUSE, TECHNO, ELECTRO, BREAKS, BASS MUSIC, HIP HOP, FUNK / WORLD, AMBIENT ...), і один вибір STYLE, який нічого не нав'язує, а лише відфільтровує всі списки й пропонує пов'язане (патерн, кіт, групу, бас, прогресію). Це підходить до принципу «одна цілісна програма» й до CHORD LAB (START FROM можна фільтрувати за стилем). Спершу варто вирішити, які стилі справді потрібні вам, а не додавати всі; тоді закрити дірки (транс, ембієнт, поп) лише тими, що знадобляться.
