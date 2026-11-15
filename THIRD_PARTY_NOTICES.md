# Composants tiers

- `third_party/jsmn/` : jsmn de Serge Zaitsev, licence MIT ; texte dans `third_party/jsmn/LICENSE` et `jsmn.h`.
- `third_party/qrcodegen/` : QR Code generator de Project Nayuki, licence MIT ; texte dans les en-têtes `qrcodegen.h` et `qrcodegen.c`.
- `romfs/ca-bundle.pem` : bundle de certificats CA utilisé pour TLS. Il contient des certificats publics ; les droits et la politique de confiance de chaque autorité sont distincts de la GPL du code.

Les outils `makerom` et `bannertool` ne sont pas redistribués ici.
