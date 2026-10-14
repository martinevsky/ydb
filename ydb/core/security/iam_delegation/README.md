# IAM delegation

This security component owns external-secret IAM integration through three separate libraries: the service and delegated-token actors in this directory, the [lifecycle coordinator and recovery workers](lifecycle/README.md), and the [durable delegation tablet](tablet/README.md). Public symbols, actor/tablet IDs and persistent formats are shared across these components.

The [external-secrets overview](docs/external-secrets/README.md) links authoritative behavior contracts, component designs, execution plans and test-owned scenario specifications. Service, lifecycle and tablet tests retain independent targets in `ut`, `lifecycle/ut` and `tablet/ut`.
