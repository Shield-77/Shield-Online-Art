# Shield Online Art

**Codex was used to generate the code**

Galerie **expérimentale** pour 3DS. L’application parcourt des tags DeviantArt, affiche des aperçus JPEG/PNG et peut présenter une image sur un plan stéréoscopique lorsque le curseur 3D est actif. Sur 2DS, l’affichage reste en 2D. Le Wi-Fi est nécessaire ; les œuvres consultées ne sont pas enregistrées comme fichiers image sur la carte SD.

(Si un jour j'arrive à stabiliser correctement l'application, peut-être qu'elle pourrait télécharger des images et vous servir pour shield 3ds theme maker ? Mais celà risque de prendre pas mal de temps, je dois étudier moi même comment fonctionne les licences et comment correctement exploiter le réseau sans provoquer de fuite de mémoire)
## Version et limites

La release actuelle est [v0.1.9](https://github.com/Shield-77/Shield-Online-Art/releases/tag/v0.1.9), fichier `ShieldOnlineArt.cia`, Title ID `000400000A210500`, version CIA interne 25. Elle limite la parallaxe stéréoscopique à 6 pixels. La recherche porte sur les **tags** : ce n’est pas une recherche libre dans tout DeviantArt. Une v0.2.0 existe localement mais n’est pas publiée dans ce dépôt.

Le [code de v0.1.9](source/), les [en-têtes](include/), le [retour OAuth](oauth/) et les [composants tiers](third_party/) sont disponibles ici. Le CIA de v0.1.9 et ce source ont été compilés ; la version reste expérimentale et les retours d’usage sont bienvenus.

## Connexion et données

L’authentification utilise un téléphone sur le même réseau et un code temporaire. Le Client ID intégré est public ; aucun mot de passe ou Client Secret n’est fourni dans le dépôt. L’application peut conserver facultativement un jeton de renouvellement dans `sdmc:/3ds/ShieldOnlineArt/refresh.dat`. Ce jeton est stocké **en clair** : ne partagez pas ce fichier ni une copie non protégée de la carte SD. Le supprimer localement ne révoque pas l’autorisation du compte DeviantArt ; cette révocation doit être faite depuis le compte. Les favoris gardent des références aux œuvres, pas leurs images.

## Compiler et droits

Installer devkitPro/devkitARM, libctru, libcurl, mbedTLS, libjpeg et libpng. Obtenir `makerom` et `bannertool` séparément, puis lancer `make cia` en indiquant leurs chemins avec `MAKEROM` et `BANNERTOOL` si nécessaire. `romfs/ca-bundle.pem` sert à la vérification TLS.

Le code original est sous [GPLv3](LICENSE). `jsmn` et le générateur QR de Project Nayuki conservent leurs licences MIT ; voir [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Les œuvres affichées restent sous les droits de leurs auteurs. Le fanart local `callie.jpg` et les exécutables de compilation ne sont pas inclus dans le source public. Codex a aidé à générer le code.
