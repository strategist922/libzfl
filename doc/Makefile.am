MAN1 =
MAN3 =
MAN7 = libzfl.7 \
    zfl_base.7 \
    zfl_bits.7 \
    zfl_blob.7 \
    zfl_config.7 \
    zfl_config_json.7 \
    zfl_config_zpl.7 \
    zfl_device.7 \
    zfl_thread.7
MAN_DOC = $(MAN1) $(MAN3) $(MAN7)

MAN_TXT = $(MAN1:%.1=%.txt)
MAN_TXT += $(MAN3:%.3=%.txt)
MAN_TXT += $(MAN7:%.7=%.txt)

if INSTALL_MAN
dist_man_MANS = $(MAN_DOC)
endif

EXTRA_DIST = asciidoc.conf $(MAN_TXT)

MAINTAINERCLEANFILES = $(MAN_DOC)

dist-hook : $(MAN_DOC)

if BUILD_DOC
SUFFIXES=.txt .xml .1 .3 .7

.txt.xml:
	asciidoc -d manpage -b docbook -f asciidoc.conf \
		-azfl_version=@PACKAGE_VERSION@ $<
.xml.1:
	xmlto man $<
.xml.3:
	xmlto man $<
.xml.7:
	xmlto man $<
endif
