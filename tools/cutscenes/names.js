// Scenario names for catalog entries, shared by docs.js and the autotest scenarios.

const scenarioName = (e) => `cut-${e.id.replace(/^@/, "").replace(/[^A-Za-z0-9_]+/g, "-")}`;
const batchName = (stage, kind) => `cut-batch-${kind}-${stage}`;

module.exports = { scenarioName, batchName };
