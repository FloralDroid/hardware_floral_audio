# Floral primary audio output HAL.
REDROID_USE_FLORAL_AUDIO := true

PRODUCT_SOONG_NAMESPACES += hardware/floral/audio

PRODUCT_PACKAGES += \
    audio.primary.floral

PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.audio.primary=floral
