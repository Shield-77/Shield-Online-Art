# SHIELD Online Art

> **EXPÉRIMENTAL — version CIA v0.1.4 disponible.** Le fonctionnement complet reste à valider sur une vraie Old 3DS/2DS.

Galerie DeviantArt destinée aux Nintendo 3DS/2DS, avec priorité à la Old 3DS. La première version vise l’affichage des œuvres en 2D.

## Version disponible

- Téléchargement : [release v0.1.4](https://github.com/Shield-77/Shield-Online-Art/releases/tag/v0.1.4).
- CIA fourni : `ShieldOnlineArt.cia`.
- v0.1.4 corrige l’empaquetage du bundle de certificats CA dans le RomFS. La vérification TLS reste activée.
- Le build a été vérifié sur PC ; la connexion DeviantArt et l’affichage doivent encore être testés sur console.

## Commandes prévues

- Barre de recherche en haut de l’écran inférieur ; toucher la barre ouvre la saisie.
- Croix directionnelle pour parcourir les résultats.
- Tactile pour sélectionner une œuvre ou une ligne de résultat.
- A pour ouvrir ; B pour revenir.

## Images et respect des œuvres

- Aucun bouton d’enregistrement ou de téléchargement des images.
- Les aperçus nécessaires à l’affichage restent temporairement en mémoire et ne sont pas copiés sur la carte SD.
- Les formats seront validés d’après le type MIME et la signature réelle du fichier. JPEG et PNG sont les décodeurs présents ; les autres formats doivent être confirmés avant d’être annoncés comme pris en charge.
- Si un aperçu n’est pas décodable, l’application doit afficher une explication plutôt que planter.

## Distribution

Ce dépôt ne publie pas le code source C de l’application. L’asset de release est le CIA. Les archives ZIP/TAR créées automatiquement par GitHub contiennent uniquement les fichiers présents dans le dépôt, notamment le README et la page de retour OAuth `oauth/index.html`.

Shield Updater v1.1.12 présente l’application comme **EXPÉRIMENTALE**. Le réseau, l’authentification et les aperçus restent à valider sur Old 3DS/2DS.
