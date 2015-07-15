# uda-plugin
Automatically exported from code.google.com/p/uda-plugin

This git repo contains an export of code.google.com/p/uda-plugin with changes added to support cxgb4/iWARP.

To build the libuda rpm:
<pre>
cd <pathto>/uda-plugin/build
QA_RPATHS=1 ./buildrpm.sh
</pre>

To install libuda:
<pre>
rpm --install ~/rpmbuild/RPMS/x86_64/libuda-3.4.1-0.1035.el6.x86_64.rpm
</pre>
