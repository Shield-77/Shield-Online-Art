# SHIELD Online Art

Galerie en ligne **expérimentale** pour 3DS et 2DS. Elle parcourt des tags DeviantArt, affiche des aperçus JPEG/PNG et peut placer l'image sur un plan stéréoscopique quand le curseur 3D est actif. Elle nécessite le Wi-Fi et ne télécharge pas les œuvres sur la carte SD.

## Version disponible

La dernière release est **[v0.1.9](https://github.com/Shield-77/Shield-Online-Art/releases/tag/v0.1.9)** (`ShieldOnlineArt.cia`, Title ID `000400000A210500`, version CIA 25). Elle ajoute l'affichage stéréoscopique plafonné à 6 pixels de parallaxe. **Le build a été compilé, mais cette version n'a pas encore été validée sur une vraie Old 3DS.** L'affichage 2DS reste en 2D.

Le code publié dans [`source/`](source/) et [`include/`](include/) correspond à **v0.1.9**. Une v0.2.0 locale est en cours de travail et n'est pas publiée ici. Le dépôt contient aussi la page de retour [`oauth/`](oauth/) et les composants tiers dans [`third_party/`](third_party/).

## Utilisation et données

La recherche actuelle fonctionne **par tag**, pas comme une recherche libre sur tout le site. L'authentification se fait avec un téléphone sur le même réseau et un code temporaire. Le Client ID présent dans le code est public ; aucun mot de passe ni Client Secret n'est fourni dans le dépôt.

Après connexion, l'application peut enregistrer **facultativement** un jeton de renouvellement dans `sdmc:/3ds/ShieldOnlineArt/refresh.dat`. Ce jeton donne accès au compte et est conservé **en clair** sur la carte SD : ne partagez ni ce fichier ni une copie de la carte. Le supprimer dans l'application n'annule pas l'autorisation côté DeviantArt ; il faut la révoquer depuis le compte si nécessaire. Les favoris enregistrent des références à des œuvres, pas leurs images.

## Compiler et licences

Installer devkitPro/devkitARM, libctru, libcurl, mbedTLS, libjpeg et libpng, puis obtenir `makerom` et `bannertool` séparément. Lancer `make cia` ; `MAKEROM` et `BANNERTOOL` permettent d'indiquer leurs chemins. Le fichier `romfs/ca-bundle.pem` est nécessaire à la vérification TLS.

Le code original de SHIELD Online Art est sous [GPLv3](Shield-Online-Art__LICENSE.txt). `jsmn` et le générateur QR de Project Nayuki gardent leurs licences MIT dans leurs fichiers sources ; voir [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Les œuvres, marques et noms affichés via DeviantArt appartiennent à leurs auteurs. Le fanart local `callie.jpg` et les exécutables de compilation ne sont pas distribués dans ce code source. Le code a été généré avec l'aide de Codex ; les fonctions restent expérimentales.
