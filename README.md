# Forge — addon Nexus

La soirée de raid de Forge (Le Bus Magique) dans le jeu, via [Raidcore Nexus](https://raidcore.gg/Nexus) : le boss en cours, ta place, l’essentiel et les mécaniques du guide, la compo, le boss suivant. Les leads mènent la soirée depuis le jeu : Lancer, Kill, Passer, Finir, aller à un boss.

## Installation

1. Nexus installé et lancé avec le jeu.
2. Télécharge `forge.dll` dans la [dernière version](https://github.com/prxmat/forge-nexus/releases/latest) et pose-le dans `Guild Wars 2\addons\`.
3. En jeu, Nexus → Addons → Forge → charger. Puis Options → Forge : colle ton **token Forge Uploader** (Forge → Mon suivi → Réglages → Forge Uploader → Créer mon token, le même que pour l’uploader) → « Enregistrer et vérifier ».
4. `Ctrl+Maj+F` (modifiable dans Nexus) ou l’icône Forge du Quick Access ouvre et ferme la fenêtre.

La fenêtre suit Forge toutes les 5 secondes : quand un lead avance (depuis le jeu, la page En direct, ou un log de kill envoyé par Forge Uploader), tout le monde le voit.

## Onglets

- **En direct** : boss en cours, ta place, l’essentiel, les mécaniques clés (premier conseil), le boss suivant.
- **Strats** : la compo demandée par le guide, ses sections (phases, CM), toutes les mécaniques avec explications et conseils : pour redonner la strat à la voix.
- **Compo** : qui tient quelle place sur ce boss, et sur le suivant : pour annoncer les changements de spé.
- **Prochain boss** : l’essentiel du suivant.

## Construire

MinGW-w64 (`brew install mingw-w64` ou `apt install mingw-w64`) puis `./build.sh` → `build/forge.dll`. ImGui 1.80 (celui de Nexus), `Nexus.h` et `Mumble.h` de Raidcore, `nlohmann/json` : tout est dans `src/`.
