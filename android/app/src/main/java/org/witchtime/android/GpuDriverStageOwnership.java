package org.witchtime.android;

/** Resolves cancellation versus delivery of one staged restart nonce exactly once. */
final class GpuDriverStageOwnership {
    private String stagedNonce;
    private boolean abandoned, delivered, cleanupClaimed;

    synchronized String stageCompleted(String nonce) {
        if (nonce == null) return null;
        if (stagedNonce != null) throw new IllegalStateException("A stage is already owned");
        stagedNonce = nonce;
        return claimCleanup();
    }

    synchronized String abandon() {
        abandoned = true;
        return claimCleanup();
    }

    synchronized boolean claimDelivery(String nonce) {
        if (abandoned || delivered || cleanupClaimed || stagedNonce == null || !stagedNonce.equals(nonce)) return false;
        delivered = true;
        return true;
    }

    private String claimCleanup() {
        if (!abandoned || delivered || cleanupClaimed || stagedNonce == null) return null;
        cleanupClaimed = true;
        return stagedNonce;
    }
}
