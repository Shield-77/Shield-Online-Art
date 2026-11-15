# SHIELD Online Art v0.1.9

## Affichage stéréoscopique

- Le curseur 3D contrôle la profondeur pendant la visualisation des images.
- La parallaxe est plafonnée à 6 pixels. Chaque image est échantillonnée et filtrée une seule fois, puis rendue pour les deux yeux. Aucun second décodage ni copie supplémentaire en RAM.
- Le mode stéréo se désactive dans les menus et pendant les chargements. Les modèles 2DS restent en 2D.
- Le guide de la galerie locale indique la commande du curseur 3D.

## Compatibilité et limites

- Nom CIA inchangé : ShieldOnlineArt.cia ; Title ID inchangé : 000400000A210500.
- Version interne CIA : 25.
- Le rendu stéréo ajoute des écritures framebuffer lorsque le curseur 3D est actif. La parallaxe reste volontairement faible pour préserver le confort et les performances de la Old 3DS.
- Compilation locale avant publication ; la lecture 3D doit être vérifiée sur une vraie Old 3DS. Le modèle 2DS ne possède pas de rendu stéréoscopique.
