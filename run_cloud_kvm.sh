#!/bin/bash
# Runs the QNX 8.0 VM + all tests under KVM on a temporary Google Cloud VM, brings results back, deletes the VM.
# Run:  bash ~/Downloads/JobAgent/qnx/run_cloud_kvm.sh
set -u
PROJ="$HOME/Downloads/JobAgent/qnx"; cd "$PROJ" || exit 1
mkdir -p logs results
LOG="logs/cloudkvm-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee "$LOG") 2>&1
ln -sf "$(basename "$LOG")" logs/latest.log
export PATH="/opt/homebrew/bin:/usr/local/bin:/opt/homebrew/share/google-cloud-sdk/bin:$PATH"
step() { echo; echo "######## $* ($(date +%H:%M:%S))"; }
fail() { echo; echo "FAILED: $*"; echo "STATUS=FAILED"; exit 1; }
VM=qnx-kvm-test; ZONE=us-central1-a

step "1. Google Cloud CLI"
command -v gcloud >/dev/null || brew install --cask google-cloud-sdk || fail "install gcloud"
export PATH="/opt/homebrew/share/google-cloud-sdk/bin:$PATH"
gcloud --version | head -1
if ! gcloud auth list --filter=status:ACTIVE --format='value(account)' | grep -q .; then
  echo ">>> A browser window will open: sign in with the Google account you used for the free trial."
  gcloud auth login --brief || fail "gcloud auth login"
fi
echo "== Your projects:"; gcloud projects list --format='table(projectId,name)'
PROJECT=${PROJECT:-$(gcloud projects list --format='value(projectId,name)' | grep -i qnx | head -1 | awk '{print $1}')}
[ -n "$PROJECT" ] || fail "no project with 'qnx' in its name or ID. Rerun as: PROJECT=<your-project-id> bash run_cloud_kvm.sh"
gcloud config set project "$PROJECT" -q
echo "using project: $PROJECT (never extern-team7 unless you pass it explicitly)"
BILLING_ON=$(gcloud billing projects describe "$PROJECT" --format='value(billingEnabled)' 2>/dev/null)
if [ "$BILLING_ON" != "True" ]; then
  ACCT=$(gcloud billing accounts list --filter=open=true --format='value(name)' | head -1)
  [ -n "$ACCT" ] || fail "no open billing account"
  echo "linking $PROJECT to billing account $ACCT"
  gcloud billing projects link "$PROJECT" --billing-account "$ACCT" || fail "link billing"
fi
echo
echo "COST CHECK: this creates one n2-standard-4 VM (about \$0.20/hour) for ~15-20 min, then deletes it."
echo "Expected charge on your card: roughly \$0.10-0.30."
read -r -p "Type yes to continue: " OK
[ "$OK" = "yes" ] || fail "cancelled by user"
gcloud services enable compute.googleapis.com -q || fail "enable Compute Engine API (is billing / free trial active?)"

step "2. Package the QNX image + binaries (QNX files stay private: copied only to your own VM)"
[ -f vm/output/ifs.bin ] && [ -f build/vibguard ] || fail "run go.sh and run_vibguard.sh first"
tar czf /tmp/qnxkvm.tgz vm/output/ifs.bin vm/output/disk-qemu vm/output/disk-qemu.vmdk build/vibguard build/timer200 build/hello \
  keys/qnx_vm_key keys/qnx_vm_key.pub data/wt901_20261005_121751.csv scripts/remote_kvm.sh || fail tar
ls -la /tmp/qnxkvm.tgz

step "3. Create VM with nested virtualization (n2-standard-4, Ubuntu 22.04)"
cleanup() { echo; echo "######## deleting VM $VM"; gcloud compute instances delete "$VM" --zone "$ZONE" -q; }
trap cleanup EXIT
gcloud compute instances create "$VM" --zone "$ZONE" --machine-type n2-standard-4 \
  --enable-nested-virtualization --image-family ubuntu-2204-lts --image-project ubuntu-os-cloud \
  --boot-disk-size 20GB -q || fail "create VM"
for i in $(seq 1 30); do gcloud compute ssh "$VM" --zone "$ZONE" -q --command true 2>/dev/null && break; sleep 5; done

step "4. Upload + run under KVM"
gcloud compute scp /tmp/qnxkvm.tgz "$VM":~ --zone "$ZONE" -q || fail scp
gcloud compute ssh "$VM" --zone "$ZONE" -q --command 'mkdir -p qnxkvm && tar xzf qnxkvm.tgz -C qnxkvm && bash qnxkvm/scripts/remote_kvm.sh' || fail "remote run"

step "5. Bring results back"
gcloud compute ssh "$VM" --zone "$ZONE" -q --command 'tar czf kvm_results.tgz kvm_results' || fail "tar results"
gcloud compute scp "$VM":~/kvm_results.tgz /tmp/ --zone "$ZONE" -q || fail "scp results"
rm -rf results/kvm && mkdir -p results/kvm && tar xzf /tmp/kvm_results.tgz -C results/kvm --strip-components 1
ls -la results/kvm
echo "STATUS=OK (VM is deleted next)"
