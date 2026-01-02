#!/bin/bash
# Optimizations aggressives pour Game Boy Emulator ESP32-S3
# Ce script modifie sdkconfig pour maximiser les performances

cd "$(dirname "$0")"

echo "🚀 Application des optimisations agressives..."

# Backup original config
cp sdkconfig sdkconfig.backup
echo "✅ Backup créé: sdkconfig.backup"

# 1. Augmenter le cache instruction: 16KB → 32KB
echo "📝 Augmentation du cache instruction: 16KB → 32KB"
sed -i.tmp 's/CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB=y/# CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB is not set/' sdkconfig
sed -i.tmp 's/# CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB is not set/CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y/' sdkconfig
sed -i.tmp 's/CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE=0x4000/CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE=0x8000/' sdkconfig

# 2. Réduire les logs: INFO → WARN (sauf pour debug)
echo "📝 Réduction du log level: INFO → WARN"
sed -i.tmp 's/CONFIG_LOG_DEFAULT_LEVEL_INFO=y/# CONFIG_LOG_DEFAULT_LEVEL_INFO is not set/' sdkconfig
sed -i.tmp 's/CONFIG_LOG_DEFAULT_LEVEL=3/CONFIG_LOG_DEFAULT_LEVEL=2/' sdkconfig
echo "CONFIG_LOG_DEFAULT_LEVEL_WARN=y" >> sdkconfig

# 3. Désactiver les assertions (déjà fait mais on s'assure)
echo "📝 Désactivation des assertions"
sed -i.tmp 's/# CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE is not set/CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y/' sdkconfig

# 4. Activer l'optimisation des tailles de lignes de cache
echo "📝 Optimisation cache line: 32B → 64B"
sed -i.tmp 's/CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_32B=y/# CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_32B is not set/' sdkconfig
sed -i.tmp 's/# CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_64B is not set/CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_64B=y/' sdkconfig
sed -i.tmp 's/CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE=32/CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE=64/' sdkconfig

sed -i.tmp 's/CONFIG_ESP32S3_DATA_CACHE_LINE_32B=y/# CONFIG_ESP32S3_DATA_CACHE_LINE_32B is not set/' sdkconfig
sed -i.tmp 's/# CONFIG_ESP32S3_DATA_CACHE_LINE_64B is not set/CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y/' sdkconfig
sed -i.tmp 's/CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=32/CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=64/' sdkconfig

# 5. Désactiver les features non utilisées
echo "📝 Désactivation des features inutilisées"
# Bluetooth (si pas utilisé)
sed -i.tmp 's/CONFIG_BT_ENABLED=y/# CONFIG_BT_ENABLED is not set/' sdkconfig 2>/dev/null || true

# Cleanup temp files
rm -f sdkconfig.tmp

echo ""
echo "✅ Optimisations appliquées!"
echo ""
echo "📊 Résumé des changements:"
echo "  • Cache instruction: 16KB → 32KB"
echo "  • Cache line: 32B → 64B"
echo "  • Log level: INFO → WARN"
echo "  • Assertions: désactivées"
echo ""
echo "🔄 Prochaines étapes:"
echo "  1. idf.py fullclean"
echo "  2. idf.py build"
echo "  3. idf.py flash monitor"
echo ""
echo "⚠️  Pour revenir en arrière: cp sdkconfig.backup sdkconfig"
