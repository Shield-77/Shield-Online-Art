# Obtenir le Client ID public

Le Client ID public **81410** a été fourni par le propriétaire et enregistré dans `include/deviantart_config.h`. Il ne donne pas à lui seul accès au compte. Aucun mot de passe, Client Secret ou jeton utilisateur ne doit être communiqué dans le chat ou placé dans une release.

1. Se connecter à son compte sur le site officiel DeviantArt, puis ouvrir https://www.deviantart.com/developers/register .
2. Donner à l'application le nom **SHIELD Online Art**. Choisir le type **Public**, adapté aux applications natives qui ne peuvent pas protéger un secret. Les nouvelles applications utilisent OAuth 2.1 avec PKCE S256.
3. Comme URI de redirection de l'application, utiliser exactement `https://shield-77.github.io/Shield-Online-Art/oauth/`. Cette adresse doit répondre avec le contenu de `oauth/index.html` publié sur GitHub Pages. Elle est aussi définie dans `include/deviantart_config.h` ; une différence, même une barre oblique finale, empêchera OAuth.
4. Enregistrer l'application et relever uniquement son **Client ID** public. Vérifier que la portée `browse` est autorisée. Le parcours utilise le téléphone pour l'autorisation puis l'adresse Wi-Fi privée affichée sur la 3DS.

La console officielle permet ensuite de vérifier un appel réel par tag sans écrire de code. Pour un client Public, son formulaire demande un champ secret même s'il n'en existe pas : la documentation autorise une valeur de remplissage `123`, utilisée uniquement pour valider le formulaire et non envoyée à DeviantArt. Le jeton obtenu reste confidentiel et ne doit pas être copié ici.

## Références officielles

- Authentification et type Public : https://deviantart.readme.io/docs/authentication
- Console de test et son URI : https://deviantart.readme.io/docs/try-the-api-in-your-browser
- Recherche par tag : https://deviantart.readme.io/reference/browse_tags

## État de la copie locale

Le Client ID public seul ne donne pas accès au compte : chaque utilisateur doit autoriser l'application via PKCE. Le transport TLS vérifie le certificat avec `romfs:/ca-bundle.pem`, qui doit être inclus dans le RomFS du CIA. Le CIA v0.1.3 ne contenait pas cette partition, car le RSF n'indiquait pas `RomFs.RootPath`. Le RSF v0.1.4 pointe vers `romfs/`, que makerom intègre dans le CIA. La requête de tag renvoie au plus huit résultats. Le filtre utilise `is_mature` et exclut les entrées dont le classement est absent ou invalide. Le parcours doit encore être retesté sur console.
