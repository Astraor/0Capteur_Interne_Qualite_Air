// CERTIFICAT EDUROAM : Aix-Marseille Université (AMU)
// Source : installeur Linux de cat.eduroam.org, profil « Eduroam-2025111701 » (serveurs eduroam.etu.univ-amu.fr
// et eduroam.univ-amu.fr, méthode TTLS-PAP : le mot de passe circule en clair DANS le tunnel chiffré, d'où
// l'importance de vérifier le serveur). Certificat public : il n'y a aucun secret dans ce fichier.
//
// Autorité retenue : « GEANT TLS ECC 1 », l'autorité INTERMÉDIAIRE qui signe les certificats des universités
// membres de GÉANT (fin de validité 31/12/2039, SHA-256 6C:DF:0B:A1:...:E4:B4:7E:C5:3C). On n'a PAS mis la racine
// « HARICA TLS ECC Root CA 2021 », aussi fournie par CAT : elle signe beaucoup plus de certificats, et ESP32
// Arduino 2.x ne vérifie pas le nom du serveur. Avec l'intermédiaire seul, il faut un certificat délivré à une
// université membre de GÉANT pour se faire passer pour le serveur d'AMU.
// Si la connexion eduroam échoue (« MDP ? » alors que le mot de passe est bon), AMU a peut-être changé
// d'autorité : retélécharger l'installeur sur cat.eduroam.org (voir ca_eduroam.h.exemple).
#pragma once
static const char CA_EDUROAM[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDNzCCArygAwIBAgIQQv3c4SYWB+Gl5pNaQAFh3TAKBggqhkjOPQQDAzBsMQsw\n"
  "CQYDVQQGEwJHUjE3MDUGA1UECgwuSGVsbGVuaWMgQWNhZGVtaWMgYW5kIFJlc2Vh\n"
  "cmNoIEluc3RpdHV0aW9ucyBDQTEkMCIGA1UEAwwbSEFSSUNBIFRMUyBFQ0MgUm9v\n"
  "dCBDQSAyMDIxMB4XDTI1MDEwMzExMTQyMVoXDTM5MTIzMTExMTQyMFowYDELMAkG\n"
  "A1UEBhMCR1IxNzA1BgNVBAoMLkhlbGxlbmljIEFjYWRlbWljIGFuZCBSZXNlYXJj\n"
  "aCBJbnN0aXR1dGlvbnMgQ0ExGDAWBgNVBAMMD0dFQU5UIFRMUyBFQ0MgMTB2MBAG\n"
  "ByqGSM49AgEGBSuBBAAiA2IABANPWLwh0Za2UqtbLV7/qNRm78zsttgSuvhn73bU\n"
  "GtxETsVOEZeMUfMjgHw8EwrsSJI9oj0CgZQFFSEY1NJfcxA/NJiOYJUKPsFbpOrY\n"
  "dr0q4g+aBZsXWeh7bMCzx24g/aOCAS0wggEpMBIGA1UdEwEB/wQIMAYBAf8CAQAw\n"
  "HwYDVR0jBBgwFoAUyRtTgRL+BNUW0aq8mm+3oJUZbsowTQYIKwYBBQUHAQEEQTA/\n"
  "MD0GCCsGAQUFBzAChjFodHRwOi8vY3J0LmhhcmljYS5nci9IQVJJQ0EtVExTLVJv\n"
  "b3QtMjAyMS1FQ0MuY2VyMBEGA1UdIAQKMAgwBgYEVR0gADAdBgNVHSUEFjAUBggr\n"
  "BgEFBQcDAgYIKwYBBQUHAwEwQgYDVR0fBDswOTA3oDWgM4YxaHR0cDovL2NybC5o\n"
  "YXJpY2EuZ3IvSEFSSUNBLVRMUy1Sb290LTIwMjEtRUNDLmNybDAdBgNVHQ4EFgQU\n"
  "6ZkGjRcfq/uWGlrIW15dXuzanI8wDgYDVR0PAQH/BAQDAgGGMAoGCCqGSM49BAMD\n"
  "A2kAMGYCMQD2M1caaY2OwmthgmANUQg3LBLI0/2LiCdxa2zNq0G59wVzbjEk0cR/\n"
  "px52OegIwRACMQCk+iTmBlR6Xfv6igiiaFiPYfN2HfbcYLWbot5DZ2H1b4JVJV+V\n"
  "rga7uu50SDG9hf4=\n"
  "-----END CERTIFICATE-----\n";
