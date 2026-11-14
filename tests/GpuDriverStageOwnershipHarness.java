package org.witchtime.android;

/** Executable host regression for chooser destruction racing staged-result delivery. */
public final class GpuDriverStageOwnershipHarness {
    private static int discardCalls;

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void discard(String nonce) {
        if (nonce != null) discardCalls++;
    }

    public static void main(String[] args) {
        GpuDriverStageOwnership lateStage = new GpuDriverStageOwnership();
        check(lateStage.abandon() == null, "nothing staged before abandonment");
        String discardAfterStage = lateStage.stageCompleted("late-stage");
        check("late-stage".equals(discardAfterStage), "stage completion after abandon must transfer nonce to cleanup");
        discard(discardAfterStage);
        check(!lateStage.claimDelivery("late-stage"), "abandoned stage must not return RESULT_OK");
        check(lateStage.abandon() == null, "cleanup ownership is exactly once");
        check(discardCalls == 1, "late stage is discarded exactly once");

        GpuDriverStageOwnership queuedResult = new GpuDriverStageOwnership();
        check(queuedResult.stageCompleted("queued-result") == null, "active stage remains deliverable");
        String discardBeforeUi = queuedResult.abandon();
        check("queued-result".equals(discardBeforeUi), "destroy before UI delivery must claim cleanup");
        discard(discardBeforeUi);
        check(!queuedResult.claimDelivery("queued-result"), "destroyed Activity must not return RESULT_OK");
        check(queuedResult.abandon() == null, "duplicate destroy cannot claim cleanup twice");
        check(discardCalls == 2, "queued result is discarded exactly once");

        GpuDriverStageOwnership delivered = new GpuDriverStageOwnership();
        check(delivered.stageCompleted("delivered") == null, "delivered stage remains active");
        check(delivered.claimDelivery("delivered"), "active chooser may deliver the nonce");
        discard(delivered.abandon());
        check(discardCalls == 2, "delivered nonce must not be discarded on destroy");
        System.out.println("GpuDriverStageOwnershipHarness: PASS");
    }
}
