#pragma once
// Vérification des certificats HTTPS par « bundle » (racines_bundle.h), à la place de celle d'Arduino-ESP32 2.x.
//
// Pourquoi la remplacer : celle d'origine (esp_crt_bundle.c) vérifie TOUJOURS la signature du haut de la chaîne reçue
// avec la clé de son émetteur. Or AirCarto et Cloudflare envoient en haut de chaîne une racine « croisée » (Root YR
// signée par ISRG Root X1, ISRG Root X2 signée par ISRG Root X1) : il fallait donc un calcul RSA 4096 bits avec la clé
// de X1, alors que la carte connaît déjà Root YR et X2. Ce calcul, au pire moment de la poignée de main, manquait de
// mémoire : « X509 - A fatal error occurred, eg the chain is too long or the vrfy callback failed » (relais).
//
// Règle ici, pour le certificat du haut de chaîne que mbedTLS n'a pas pu rattacher (drapeau NOT_TRUSTED seul) :
//  1. s'il a EXACTEMENT le nom ET la clé publique d'une racine du bundle, c'est cette racine (version croisée) : on
//     lui fait confiance, comme à la racine elle-même. Sûr : la confiance porte sur la clé ; toutes les signatures en
//     dessous restent vérifiées par mbedTLS avec cette clé (personne d'autre ne peut signer avec) ;
//  2. sinon, son émetteur doit être une racine du bundle (même nom, comparé en entier) et sa signature doit être
//     valide avec la clé de cette racine (comme la version d'origine) ;
//  3. sinon : refus (erreur fatale, connexion fermée).
// Les autres vérifications de mbedTLS ne changent pas : signatures de toute la chaîne, autorités (CA), usage des clés,
// nom du serveur (un mauvais nom laisse un autre drapeau : la fonction ne touche alors à rien et la connexion échoue).
//
// Branchement : platformio.ini relie arduino_esp_crt_bundle_attach (appelée par WiFiClientSecure) à
// __wrap_arduino_esp_crt_bundle_attach ci-dessous (option d'édition de liens --wrap). Si ce lien manquait, la version
// d'origine trouverait un bundle vide et refuserait toute connexion : jamais d'ouverture sans vérification.
#include <string.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/md.h>

// Bundle utilisé par la prochaine connexion (réglé juste avant, sous le verrou mutexTls).
static const uint8_t* volatile bundleCourant = nullptr;
static volatile uint32_t nbVerifsBundle = 0;          // appels (diagnostic : prouve que c'est bien cette fonction)

static bool signatureValideParCle(const mbedtls_x509_crt* enfant, const uint8_t* cle, size_t lgCle) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  bool ok = false;
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(enfant->sig_md);
  unsigned char h[MBEDTLS_MD_MAX_SIZE];
  if (md != nullptr && mbedtls_pk_parse_public_key(&pk, cle, lgCle) == 0 && mbedtls_pk_can_do(&pk, enfant->sig_pk)
      && mbedtls_md(md, enfant->tbs.p, enfant->tbs.len, h) == 0) {
    ok = mbedtls_pk_verify_ext(enfant->sig_pk, enfant->sig_opts, &pk, enfant->sig_md, h, mbedtls_md_get_size(md),
                               enfant->sig.p, enfant->sig.len) == 0;
  }
  mbedtls_pk_free(&pk);
  return ok;
}

static int verifierParBundle(void*, mbedtls_x509_crt* crt, int, uint32_t* drapeaux) {
  nbVerifsBundle++;
  if ((*drapeaux & ~(uint32_t)MBEDTLS_X509_BADCERT_BAD_MD) != MBEDTLS_X509_BADCERT_NOT_TRUSTED) return 0;
  const uint8_t* b = bundleCourant;
  if (b == nullptr) return MBEDTLS_ERR_X509_FATAL_ERROR;
  const unsigned nb = (unsigned)b[0] << 8 | b[1];
  const uint8_t* p = b + 2;
  const uint8_t* cleEmetteur = nullptr;
  size_t lgCleEmetteur = 0;
  for (unsigned i = 0; i < nb; i++) {
    const size_t lgNom = (size_t)p[0] << 8 | p[1], lgCle = (size_t)p[2] << 8 | p[3];
    const uint8_t* nom = p + 4;
    const uint8_t* cle = nom + lgNom;
    if (lgNom == crt->subject_raw.len && lgCle == crt->pk_raw.len && memcmp(nom, crt->subject_raw.p, lgNom) == 0
        && memcmp(cle, crt->pk_raw.p, lgCle) == 0) {
      *drapeaux = 0;                                   // 1. c'est une racine du bundle (même nom, même clé)
      return 0;
    }
    if (lgNom == crt->issuer_raw.len && memcmp(nom, crt->issuer_raw.p, lgNom) == 0) {
      cleEmetteur = cle;
      lgCleEmetteur = lgCle;
    }
    p = cle + lgCle;
  }
  if (cleEmetteur != nullptr && signatureValideParCle(crt, cleEmetteur, lgCleEmetteur)) {
    *drapeaux = 0;                                     // 2. signé par une racine du bundle
    return 0;
  }
  return MBEDTLS_ERR_X509_FATAL_ERROR;                 // 3. refus
}

// Appelée par ssl_client.cpp (WiFiClientSecure) quand setCACertBundle est actif, à la place de l'originale.
static mbedtls_x509_crt crtFactice;                    // liste de confiance vide : tout passe par verifierParBundle
extern "C" int __wrap_arduino_esp_crt_bundle_attach(void* conf) {
  static bool pret = false;
  if (!pret) { mbedtls_x509_crt_init(&crtFactice); pret = true; }
  if (conf == nullptr || bundleCourant == nullptr) return -1;   // < 0 : la connexion échoue
  mbedtls_ssl_conf_ca_chain((mbedtls_ssl_config*)conf, &crtFactice, nullptr);
  mbedtls_ssl_conf_verify((mbedtls_ssl_config*)conf, verifierParBundle, nullptr);
  return 0;
}
