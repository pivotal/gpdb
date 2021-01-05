#!/bin/bash
set -ex

CWDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DIR="$CWDIR"
# dummy version & release for sample package
VERSION=0.0
RELEASE=1

function determine_os() {
  local name version
  if [ -f /etc/redhat-release ]; then
    name="centos"
    version=$(sed </etc/redhat-release 's/.*release *//' | cut -f1 -d.)
  elif [ -f /etc/SuSE-release ]; then
    name="sles"
    version=$(awk -F " *= *" '$1 == "VERSION" { print $2 }' /etc/SuSE-release)
  elif  grep -q photon /etc/os-release  ; then
    name="photon"
    version=$( awk -F " *= *" '$1 == "VERSION_ID" { print $2 }' /etc/os-release | cut -f1 -d.)
  elif grep -q ubuntu /etc/os-release ; then
    name="ubuntu"
    version=$(awk -F " *= *" '$1 == "VERSION_ID" { print $2 }' /etc/os-release | tr -d \")
  else
    echo "Could not determine operating system type" >/dev/stderr
    exit 1
  fi
  echo "${name}${version}"
}
OS=`determine_os`
ARCH=`uname -m`

DESTINATION_GPPKG_PATH=/tmp/sample-gppkg
rm -rf ${DESTINATION_GPPKG_PATH}
mkdir -p ${DESTINATION_GPPKG_PATH}/deps

function buildNativePackage() {

BUILDROOT=/tmp/package-build
rm -rf ${BUILDROOT}
case "$OS" in
centos*|rhel*|photon*)
# We assume that these rpms have already been installed
#yum -y install rpmdevtools rpmlint
	mkdir -p ${BUILDROOT}/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
	cp ${DIR}/sample-sources.tar.gz ${BUILDROOT}/SOURCES/
	cp ${DIR}/sample.spec ${BUILDROOT}/SPECS/
	rpmbuild -bb ${BUILDROOT}/SPECS/sample.spec --define "%_topdir ${BUILDROOT}" --define "debug_package %{nil}"
	cp ${BUILDROOT}/RPMS/x86_64/*.rpm ${CWDIR}/data/
;;
ubuntu*)
	if [ "$ARCH" == "x86_64" ];then
		ARCH=amd64
	fi
	PACKAGENAME=sample-0.0-1.${ARCH}.deb
	mkdir -p ${BUILDROOT}/UBUNTU/DEBIAN
	cat ${DIR}/sample.control | sed "s|#version|${VERSION}-${RELEASE}|" | sed "s|#arch|${ARCH}|" > ${BUILDROOT}/UBUNTU/DEBIAN/control
	mkdir -p ${BUILDROOT}/UBUNTU/bin
	mkdir -p ${BUILDROOT}/UBUNTU/include
	mkdir -p ${BUILDROOT}/UBUNTU/lib
	mkdir -p ${BUILDROOT}/UBUNTU/share/postgresql
	touch ${BUILDROOT}/UBUNTU/bin/sample
	touch ${BUILDROOT}/UBUNTU/lib/sample.so
	touch ${BUILDROOT}/UBUNTU/include/sample.h
	touch ${BUILDROOT}/UBUNTU/share/postgresql/sample.sql
	dpkg-deb --build ${BUILDROOT}/UBUNTU "${BUILDROOT}/${PACKAGENAME}"
	mv ${BUILDROOT}/${PACKAGENAME} ${CWDIR}/data/
;;
*)
	echo "unkown OS <$OS>" >&2
	env
	exit 1;
;;
esac
}

function buildGppkg() {
case "$OS" in
centos*|photon*)
	cp ${CWDIR}/data/sample*.rpm ${DESTINATION_GPPKG_PATH}/
;;
ubuntu*)
	cp ${CWDIR}/data/sample*.deb ${DESTINATION_GPPKG_PATH}/
;;
*)
	echo "unkown OS <$OS>" >&2
	exit 1;
;;
esac

cat ${DIR}/gppkg_spec.yml.in | sed "s/#arch/${ARCH}/g" | sed "s/#os/${OS}/g" | sed "s/#gppkgver/${VERSION}.${RELEASE}/g" > ${DESTINATION_GPPKG_PATH}/gppkg_spec.yml.in

make -f ${CWDIR}/build_gppkg.mk -C ${DESTINATION_GPPKG_PATH} gppkg

# --define "%_topdir ${BUILDROOT}" tells where to find SOURCES
#pushd ${DESTINATION_GPPKG_PATH}
#tar czf sample.gppkg *
#popd
}

case "$1" in
buildNative)
	buildNativePackage ;;
buildGppkg)
	buildGppkg ;;
*)
	echo "usage: $0 {buildNative | buildGppkg}"
	;;
esac
