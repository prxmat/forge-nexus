# Forge — addon Nexus

La soirée de raid de Forge (Le Bus Magique) dans le jeu, via [Raidcore Nexus](https://raidcore.gg/Nexus) : le boss en cours, ta place, l’essentiel et les mécaniques du guide, la compo, le boss suivant. Les leads mènent la soirée depuis le jeu : Lancer, Kill, Passer, Finir, aller à un boss.

## Installation

1. Nexus installé et lancé avec le jeu.
2. Télécharge `forge.dll` dans la [dernière version](https://github.com/prxmat/forge-nexus/releases/latest) et pose-le dans `Guild Wars 2\addons\`.
3. En jeu, Nexus → Addons → Forge → charger. Puis Options → Forge : colle ton **token Forge Uploader** (Forge → Mon suivi → Réglages → Forge Uploader → Créer mon token, le même que pour l’uploader) → « Enregistrer et vérifier ».
4. `Ctrl+Maj+F` (modifiable dans Nexus) ou l’icône Forge du Quick Access ouvre et ferme la fenêtre.

La fenêtre suit Forge toutes les 5 secondes : quand un lead avance (depuis le jeu, la page En direct, ou un log de kill envoyé par Forge Uploader), tout le monde le voit.

## Onglets

- **Combats** : chaque log lu **en local dès qu’arcdps l’écrit**, sans attendre l’envoi. McM : un onglet par équipe (couleur), totaux (joueurs, kills, morts, à terre, dégâts), une ligne par spécialisation (compte, icône, dégâts, barre à la couleur de la classe), « Affichage » (tri par dégâts, escouade seulement, noms courts, widget) et « Historique » (30 combats). PvE : boss, kill ou wipe (% restant), ton DPS et ton rang, morts de l’escouade, dégâts par joueur en barres.
- **Widget** : une ligne près du bord de l’écran avec le dernier combat : effectifs des équipes empilés en couleur et kills/morts en McM, boss / résultat / ton DPS et ton rang en PvE. Raccourci `KB_FORGE_WIDGET` dans Nexus, option « masquer en combat ».
- **Nouvelle sortie McM** (Stats Forge) : les combats envoyés ensuite vont dans une nouvelle sortie sur Forge ; Forge en ouvre aussi une après 2 h sans combat.

- **Stats Forge** : ce que Forge a calculé du dernier essai et de la sortie McM (revue, stabilité, constats), 20 à 40 s après l’envoi par Forge Uploader.
- **En direct** : boss en cours, ta place, l’essentiel, les mécaniques clés (premier conseil), le boss suivant.
- **Strats** : la compo demandée par le guide, ses sections (phases, CM), toutes les mécaniques avec explications et conseils : pour redonner la strat à la voix.
- **Compo** : qui tient quelle place sur ce boss, et sur le suivant : pour annoncer les changements de spé.
- **Prochain boss** : l’essentiel du suivant.

## Alertes Forge

Des timers de boss, des auras d’avantages et une voix, dessinés par-dessus le jeu comme WeakAuras : affichage et son seulement, rien n’appuie sur une touche à ta place.

- **Timers de boss** au format Blish HUD / TaimiHUD (`.bhtimer`), celui de [Hero’s Timers](https://github.com/QuitarHero/Hero-Timers) (QuitarHero). Forge lit `addons\Forge\timers` (onglet Alertes → « Télécharger Hero’s Timers » y installe la dernière version) et `addons\Taimi\timers` si TaimiHUD en a déjà. Chaque timer suit le combat comme dans TaimiHUD : zone, entrée en combat, phases, réarmement hors combat. Les **avertissements** sont des barres qui se vident jusqu’au moment annoncé (icône du pack, secondes restantes, cadre rouge les 3 dernières) ; les **alertes** s’affichent en grand au centre de l’écran, avec le compte à rebours des dernières secondes. `skipTime` est pris en compte. Une alerte reste 5 s au centre au plus, puis continue en barre. Les timers d’entraînement « Simulation » (mode défi du Temple des moissons) sont désactivés tant qu’on ne les coche pas : ils démarrent hors combat et gardent « Begin Simulation » affiché jusqu’à la touche 0.
- **Voix** (synthèse vocale de Windows) : les annonces du timer, ses alertes, ses avertissements N secondes avant (3 par défaut), une voix anglaise pour les textes anglais des packs ; bip à l’échéance en option.
- **Touches de timer** : les timers « Hotkey » de Hero’s Timers (Deimos, Dhuum Shackles, Temple de Febe, Ura…) attendent la « Trigger Key 0 à 4 » : à assigner dans Nexus → Raccourcis (« Forge · Alertes : touche de timer 0 à 4 », et « réarmer les timers »).
- **Auras** : tes avantages (célérité, alacrité, pouvoir, stabilité… treize au choix) en icônes, durée en balayage et en secondes, stacks dans le coin ; cadre rouge qui pulse et voix ou bip quand l’un manque en combat (après 1,5 s, au plus toutes les 6 s). Minimum de stacks pour le pouvoir et la stabilité. Elles viennent des événements d’avantages d’arcdps en temps réel.
- « Déplacer les barres et les auras » pour les placer (onglet Alertes, ou clic droit sur le panneau du combat, qui ouvre aussi la fenêtre Forge) ; « Tester l’affichage et la voix » pour voir le rendu sans être en combat.

## Construire

MinGW-w64 (`brew install mingw-w64` ou `apt install mingw-w64`) puis `./build.sh` → `build/forge.dll`. ImGui 1.80 (celui de Nexus), `Nexus.h` et `Mumble.h` de Raidcore, `nlohmann/json`, `miniz` : tout est dans `src/`.

Vérifications natives (clang, sans Windows) : `tools/timers-check.cpp` charge un pack de timers et joue Slothasor, les huiles de Deimos et un `skipTime` ; `tools/auras-check.cpp` rejoue des événements d’avantages arcdps (format 2025 et ancien). Les lignes de compilation sont en tête de chaque fichier.

## Crédits

La lecture des logs arcdps (format evtc, équipes McM par couleur, comptes par spécialisation) suit [WvW Fight Analysis](https://github.com/jake-greygoose/WvW-Fight-Analysis-Addon) de jake-greygoose (MIT), dont viennent aussi les icônes de spécialisation (`src/spec_icons.h`) et `miniz`. Les Alertes suivent le format et la machine à états de [TaimiHUD](https://github.com/TaimiHUD/TaimiHUD) et lisent les timers de [Hero’s Timers](https://github.com/QuitarHero/Hero-Timers). Merci.
