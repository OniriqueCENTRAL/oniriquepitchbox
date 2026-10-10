# Onirique PitchBox

Éditeur de hauteur en **ARA 2** pour la voix : le plugin reçoit la prise entière,
l'analyse note par note, et resynthétise à la lecture. Une note déplacée dans
l'éditeur est entendue au buffer suivant, sans réanalyse et sans re-rendu du take.

VST3 (Windows, macOS, Linux) et Audio Unit (macOS).

---

## État du projet

| Partie | État |
|---|---|
| Détection de hauteur (YIN accéléré par FFT) | fait, testé |
| Marques de période et PSOLA à formants préservés | fait, testé |
| Segmentation en notes, détection de tonalité | fait, testé |
| Correction par gamme + édition par note | fait, testé |
| Rendu temps réel bloc par bloc, bit-exact | fait, testé |
| Intégration ARA 2 (analyse, rendu, persistance) | écrit, compile |
| Interface d'édition des notes | écrite, compile |
| Polyphonie (édition de notes dans un accord) | non commencé |
| Édition du timing (déplacer une note dans le temps) | non commencé |

**Ce qui n'a pas été vérifié :** le projet a été développé et compilé sous Linux.
Les binaires macOS n'ont pas pu être produits ni chargés dans un hôte — Apple
exige une compilation sur macOS. Tout le code du plugin compile avec JUCE 9.0.3
et le SDK ARA 2, et le chemin audio est validé hors ligne (voir *Tests*), mais
le comportement dans un vrai DAW reste à confirmer.

---

## Construire sur macOS

Prérequis : Xcode (outils en ligne de commande suffisants pour la compilation),
CMake ≥ 3.22.

```bash
git clone --depth 1 https://github.com/juce-framework/JUCE.git
git clone --recursive --depth 1 https://github.com/Celemony/ARA_SDK.git
```

Le `--recursive` n'est pas optionnel : `ARA_API` et `ARA_Library` sont des
sous-modules, et sans eux la compilation échoue sur `ARA_Library/Debug/ARADebug.h`.
Si le dépôt a déjà été cloné sans :

```bash
cd ARA_SDK && git submodule update --init --depth 1 ARA_API ARA_Library
```

Puis, depuis la racine du projet :

```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

Les binaires sont installés automatiquement (`COPY_PLUGIN_AFTER_BUILD`) dans :

```
~/Library/Audio/Plug-Ins/VST3/PitchBox.vst3
~/Library/Audio/Plug-Ins/Components/PitchBox.component
```

Pour un build universel Intel + Apple Silicon, c'est déjà le défaut
(`CMAKE_OSX_ARCHITECTURES = arm64;x86_64`).

### Signature et distribution

Pour distribuer en dehors de ta machine, les binaires doivent être signés et
notarisés, sinon Gatekeeper les bloquera chez tes clients :

```bash
codesign --force --deep --options runtime --timestamp \
  --sign "Developer ID Application: <ton identité>" \
  ~/Library/Audio/Plug-Ins/VST3/PitchBox.vst3
```

puis notarisation via `notarytool` sur un `.pkg` ou un `.dmg`.

### Sur Windows

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Sur Linux

Dépendances : `libasound2-dev libgl1-mesa-dev libx11-dev libxext-dev
libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev
libfreetype6-dev libfontconfig1-dev`.

---

## Hôtes

ARA doit être supporté par le DAW *et* activé sur la piste. Le plugin s'insère
comme extension ARA, pas comme insert classique. Hôtes compatibles ARA 2 :
Studio One, Logic Pro, Cubase / Nuendo, REAPER, Cakewalk, Mixcraft.

### Hôtes sans ARA (FL Studio, Ableton Live…) : mode Transfer

Sans ARA, personne ne donne la prise au plugin : il faut la lui montrer une
fois. Inséré comme effet sur la piste de la voix :

1. cliquer **TRANSFER** (le bouton passe à *CANCEL*, le plugin est armé) ;
2. lancer la lecture dans l'hôte sur le passage à corriger ;
3. arrêter la lecture (ou cliquer **STOP**) : le passage est analysé.

Le plugin rejoue ensuite sa propre copie corrigée à la place de la piste chaque
fois que la tête de lecture traverse ce passage ; en dehors, la piste passe
telle quelle. Un nouveau transfert remplace le précédent. La prise est
enregistrée dans le projet (FLAC 24 bits), avec les éditions.

Limites : une seule prise par instance ; elle est calée sur sa position dans le
morceau, donc si le clip est déplacé ensuite il faut refaire le transfert ; et
elle n'est rejouée qu'à la fréquence d'échantillonnage où elle a été
enregistrée. Le code est dans `Source/transfer/`, testé par `test_transfer`.

---

## Comment c'est construit

```
Source/dsp/          traitement du signal, sans dépendance
  Fft                FFT radix-2
  YinPitchTracker    f0 par YIN, différence évaluée par corrélation FFT
  PitchMarks         ancrage des grains sur les impulsions glottales
  Psola              resynthèse temps-réel à formants préservés
  Scale              gammes, snap, détection de tonalité (profils Krumhansl)

Source/model/        le modèle, sans dépendance
  Notes              segmentation en notes, courbe de correction
  Analysis           une prise analysée, immuable
  EditState          ce que l'utilisateur a changé, + sérialisation
  RegionRenderer     la moitié « thread audio » du plugin

Source/ara/          intégration ARA
Source/ui/           interface
```

### Les trois décisions qui structurent tout

**L'analyse et les éditions sont séparées.** `Analysis` décrit ce qui a été
chanté et ne change jamais ; `EditState` décrit ce qu'on veut à la place. Une
édition ne touche donc pas à l'analyse — c'est précisément pour ça qu'elle
s'entend au buffer suivant. Inversement, une réanalyse (punch-in, changement
d'algorithme) peut être intégrée sans jeter le travail de l'utilisateur : les
éditions sont rattachées aux nouvelles notes par recouvrement temporel, et
exprimées en écart relatif à la hauteur détectée.

**Les ancres de phase.** PSOLA pose ses grains en avançant un curseur, et
l'endroit où cette marche commence détermine la phase partout ensuite. Un
renderer naïf repart du début du fichier : coût proportionnel à la longueur de
la prise, inacceptable sur le thread audio. Les ancres sont des points de remise
à zéro déterministes, posés sur des marques de période. Comme la position d'une
ancre ne dépend que de l'analysis, jamais du découpage en buffers de l'hôte, le
même échantillon sort identique qu'on le demande en un bloc, en quarante, ou
pendant un export hors ligne.

Une remise à zéro n'est pas gratuite : dès que la hauteur est modifiée, elle
raccourcit le cycle en cours. Les ancres ne sont donc posées que là où ça ne
s'entend pas — quand la voix s'arrête, dans un souffle ou une consonne — et
sinon au plus une toutes les deux secondes dans un passage voisé plus long que
ça. Surtout pas aux débuts de notes ni à intervalle court : sur une vraie voix
cela faisait ~45 cycles tronqués par seconde, un flutter sur tout le signal.

**La marche reprise.** Des ancres espacées rendraient chaque bloc coûteux s'il
fallait repartir de la dernière à chaque fois. Le renderer retient donc où en
était le curseur et reprend de là au bloc suivant ; et il demande le ratio de
correction grain par grain au lieu de remplir une courbe échantillon par
échantillon sur toute la marche.

### Contrat temps réel

Après `prepare()`, `RegionRenderer::render()` n'alloue rien et son coût est
borné par la longueur du bloc plus une marche arrière fixe — indépendamment de
la durée de la prise. Mesures sur une prise de 7,8 s (voir `test_renderer`) :

```
bloc   64 :  29x temps réel
bloc  256 :  68x
bloc  512 :  89x
bloc 2048 : 127x
```

---

## Tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target test_yin test_psola test_notes test_realtime test_renderer
ctest --test-dir build --output-on-failure
```

Les tests n'ont besoin ni de JUCE ni d'un hôte : ils exercent le chemin audio
complet hors ligne. Les deux qui comptent le plus :

- `test_realtime` et `test_renderer` rendent la même prise bloc par bloc, chaque
  bloc à froid depuis une fenêtre de source, et comparent au rendu hors ligne
  complet. L'accord est exact (erreur nulle) à 64, 256, 512 et 2048 échantillons,
  et en démarrant la lecture à trois endroits différents. C'est la propriété
  dont dépend un plugin ARA : sans elle, un export ne correspondrait pas à ce
  qu'on a entendu.
- `test_notes` vérifie qu'une correction complète met les notes en place
  (pire cas 10 cents, contre 45 avant) **sans écraser le vibrato** (profondeur
  52 → 52 cents), et qu'un `vibratoScale` à 0 l'aplatit bien (52 → 5).

---

## Utilisation

- **Glisser une note** verticalement : la poser sur la gamme. `Alt` pour un
  placement libre.
- **Maintenir le clic sur une note** : l'entendre tenue, corrigée, à l'endroit
  où on la tient (transport arrêté, mode Transfer uniquement). Glissée, elle
  s'entend bouger.
- **Sélection multiple** : `Ctrl` ou `Maj` + clic, ou un cadre tracé dans le
  vide. Glisser, double-cliquer ou régler MODULATION agit sur toute la sélection.
- **Double-clic** : exclure la note de la correction (pour un bend voulu).
- **MODULATION** : sur les notes sélectionnées, ce qu'on garde du mouvement
  chanté à l'intérieur de la note (vibrato, attaque, dérive). 0 aplatit le
  trait, 1 le laisse tel que chanté. Double-clic sur le potard : retour à 1.
- **UNDO / REDO / RESET** : RESET efface les éditions faites à la main et garde
  les réglages globaux ; il s'annule comme le reste.
- **Molette** : monter et descendre sur tout le clavier. `Maj` + molette :
  avancer et reculer dans la prise. `Ctrl` + molette : zoom dans le temps.
  `Ctrl` + `Maj` + molette (ou `Alt` + molette) : zoom en hauteur. Les deux
  zooms se font sur le curseur.
- **Barres de défilement** en bas et à droite, et **glisser au clic droit ou
  molette** pour déplacer la vue dans les deux sens.
- **AMOUNT** : force de la correction sur les notes non touchées à la main.
- **SPEED** : durée de la transition en début et fin de note. 0 donne le snap
  dur et audible ; 40–80 ms sonne comme un chanteur qui trouve sa note.
- **FORMANT** : déplace l'enveloppe spectrale indépendamment de la hauteur.
- **DETECT KEY** : règle la gamme sur la tonalité trouvée dans la prise.

Le trait vertical est la tête de lecture de l'hôte ; pendant la lecture la vue
tourne la page pour le suivre.

La courbe grise est ce qui a été chanté, la courbe braise ce qui sortira. Le
contour fantôme derrière une note montre d'où elle vient.

---

## Licences

- **SDK ARA 2** (Celemony) : Apache 2.0. Aucune contrainte sur la distribution.
- **JUCE 9** : double licence **AGPLv3 ou licence commerciale**. L'AGPLv3
  obligerait à publier le code source de PitchBox. Pour une distribution
  payante à code fermé, une licence JUCE commerciale est donc nécessaire.
  Je ne suis pas juriste : à confirmer auprès de JUCE ou d'un conseil.
- Le code de PitchBox lui-même : à toi.

### Direct Note Access

L'édition de notes individuelles **dans un accord** (le DNA de Melodyne) n'est
pas implémentée, et ne relève pas de la même famille de techniques : c'est de la
séparation de sources au niveau de la note. Celemony détient par ailleurs des
brevets déposés autour de 2005-2010 sur ce terrain ; certains arrivent à
expiration, mais c'est à faire vérifier par un conseil en propriété
intellectuelle avant toute commercialisation d'une fonction équivalente.

---

## Suites possibles

1. **Édition du timing** — déplacer et étirer une note dans le temps. La
   structure est prête : il faut passer `psolaRender` d'un ratio de hauteur à un
   couple (hauteur, position), et déclarer l'étirement à ARA pour que l'hôte
   recale les régions.
2. **Analyse incrémentale** — ne réanalyser que la portion modifiée après un
   punch-in, au lieu de la prise entière.
3. **Polyphonie** — transcription multi-f0, puis séparation par note.
