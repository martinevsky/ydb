import argparse
import logging
import random
import time
from concurrent import futures
import os

import grpc
from google.protobuf import timestamp_pb2
from google.rpc import error_details_pb2
from google.rpc import status_pb2

from ydb.public.api.client.yc_private.iam import iam_token_service_pb2
from ydb.public.api.client.yc_private.iam import iam_token_service_pb2_grpc
from ydb.public.api.client.yc_private.iam import operation_service_pb2_grpc
from ydb.public.api.client.yc_private.iam import service_account_pb2
from ydb.public.api.client.yc_private.iam import service_account_service_pb2_grpc
from ydb.public.api.client.yc_private.iam import service_control_service_pb2_grpc
from ydb.public.api.client.yc_private.operation import operation_pb2
from ydb.public.api.client.yc_private.resourcemanager import folder_pb2
from ydb.public.api.client.yc_private.resourcemanager import folder_service_pb2
from ydb.public.api.client.yc_private.resourcemanager import folder_service_pb2_grpc

if os.environ.get("USE_ACCESS_SERVICE_V2", "true") == "true":
    from ydb.public.api.client.yc_private.accessservice import access_service_pb2
    from ydb.public.api.client.yc_private.accessservice import access_service_pb2_grpc
else:
    from ydb.public.api.client.yc_private.servicecontrol import access_service_pb2
    from ydb.public.api.client.yc_private.servicecontrol import access_service_pb2_grpc

logger = logging.getLogger(__name__)
logging.basicConfig(level=logging.DEBUG)

# PRNG
random.seed(0)

DEFAULT_TOKEN = "root@builtin"
DEFAULT_EXPIRES_IN = 3600

# subject returned by AccessService.Authenticate for any token (SID "<SUBJECT_ID>@as" inside YDB)
SUBJECT_ID = "bob"

# Delegations set up through ServiceControlService.SetupDelegation: (resource_id, target_sa) -> set of referrer ids.
# IamTokenService.CreateForService requires an active delegation for target service accounts named "delegated-*".
DELEGATIONS = {}
DELEGATION_TARGET_PREFIX = "delegated-"

# ServiceAccountService.Get and FolderService.Resolve answer only to a user's token (one starting with
# this prefix), never to the token of the YDB service itself, as the real IAM does. A service account
# "<sa>" lives in folder "folder-of-<sa>" of cloud "cloud-of-<sa>", except:
#   "delegated-nofolder*"  the service account is not found
#   "delegated-nocloud*"   the folder of the service account does not resolve to a cloud
USER_TOKEN_PREFIX = "cloud-user-token"


def bearer_token(context):
    for key, value in context.invocation_metadata():
        if key == "authorization":
            return value[len("Bearer "):] if value.startswith("Bearer ") else value
    return ""


def check_user_token(context, method):
    token = bearer_token(context)
    if not token.startswith(USER_TOKEN_PREFIX):
        logger.debug("%s refused for token=%s: not a user token", method, token)
        context.set_code(grpc.StatusCode.PERMISSION_DENIED)
        context.set_details("You are not authorized for this operation")
        return False
    return True


# ServiceControl is called by YDB as itself, never with the user's token: a user's bearer is refused,
# as the real IAM does (no customer user holds the delegator role on the gizmo of the service).
def check_service_token(context, method):
    token = bearer_token(context)
    if not token or token.startswith(USER_TOKEN_PREFIX):
        logger.debug("%s refused for token=%s: not the token of the service", method, token)
        context.set_code(grpc.StatusCode.UNAUTHENTICATED)
        context.set_details("The token is not the token of a system service account")
        return False
    return True


# Fails the call the way IAM reports a ServiceControlFailureType: a google.rpc.PreconditionFailure
# violation of the given type in the error details (the grpc-status-details-bin trailer).
def fail_with_precondition(context, code, message, failure_type):
    failure = error_details_pb2.PreconditionFailure()
    violation = failure.violations.add()
    violation.type = failure_type
    violation.subject = "service-control"
    violation.description = "emulated " + failure_type
    status = status_pb2.Status(code=code.value[0], message=message)
    status.details.add().Pack(failure)
    context.set_trailing_metadata((("grpc-status-details-bin", status.SerializeToString()),))
    context.set_code(code)
    context.set_details(message)


def parse_args():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--port", type=int, required=True, help="gRPC port to listen on")
    parser.add_argument("--token", type=str, default=DEFAULT_TOKEN, help="IAM token to return")
    parser.add_argument("--token-from-env", type=str, help="Use IAM token from environment (preferred for secret passing)")
    parser.add_argument("--expires-in", type=int, default=DEFAULT_EXPIRES_IN,
                        help="Token TTL in seconds")
    return parser.parse_args()


def make_response(token, expires_in):
    expires_at = timestamp_pb2.Timestamp()
    expires_at.seconds = int(time.time()) + expires_in
    return iam_token_service_pb2.CreateIamTokenResponse(
        iam_token=token,
        expires_at=expires_at,
    )


class IamTokenServicer(iam_token_service_pb2_grpc.IamTokenServiceServicer):
    def __init__(self, token, expires_in):
        self.token = token
        self.expires_in = expires_in
        self.calls = 0
        self.token_calls = 0

    def _pick_token(self):
        return self.token

    def Create(self, request, context):
        logger.debug("IamTokenService.Create called")
        return make_response(self._pick_token(), self.expires_in)

    def CreateForServiceAccount(self, request, context):
        logger.debug("IamTokenService.CreateForServiceAccount called, sa_id=%s", request.service_account_id)
        return make_response(self._pick_token(), self.expires_in)

    def CreateForService(self, request, context):
        token = self._pick_token()
        logger.debug(
            "IamTokenService.CreateForService called, service_id=%s microservice_id=%s resource_id=%s target_sa=%s token=%s",
            request.service_id, request.microservice_id, request.resource_id, request.target_service_account_id, token
        )

        target_sa = request.target_service_account_id
        expires_in = self.expires_in

        if target_sa == 'flaky':
            if random.random() < 0.1:
                logger.debug("Simulating random failure")
                context.set_code(grpc.StatusCode.UNAVAILABLE)
                context.set_details("Too busy to respond")
                return iam_token_service_pb2.CreateIamTokenResponse()

        if target_sa == 'bad':
            context.set_code(grpc.StatusCode.PERMISSION_DENIED)
            context.set_details("Reject bad SA")
            return iam_token_service_pb2.CreateIamTokenResponse()

        if target_sa == 'bad-token':
            return make_response("badtoken@builtin", expires_in)

        if target_sa == 'unavailable-token':
            context.set_code(grpc.StatusCode.UNAVAILABLE)
            context.set_details("Too busy to respond forever")
            return iam_token_service_pb2.CreateIamTokenResponse()

        if target_sa == 'slow-unavailable-token':
            time.sleep(30)
            context.set_code(grpc.StatusCode.UNAVAILABLE)
            context.set_details("Too busy and slow to respond forever")
            return iam_token_service_pb2.CreateIamTokenResponse()

        if target_sa.startswith('bad-skip-'):
            skips = int(target_sa.split('-')[-1])
            self.calls += 1
            self.calls %= skips + 1
            expires_in = 0
            if self.calls == 0:
                context.set_code(grpc.StatusCode.PERMISSION_DENIED)
                context.set_details("Reject bad SA")
                return iam_token_service_pb2.CreateIamTokenResponse()

        if target_sa.startswith('bad-token-skip-'):
            skips = int(target_sa.split('-')[-1])
            self.token_calls += 1
            self.token_calls %= skips + 1
            expires_in = 0
            if self.token_calls == 0:
                return make_response("badtoken@builtin", expires_in)

        if target_sa == 'bad-token':
            return make_response("badtoken@builtin", expires_in)

        if target_sa.startswith(DELEGATION_TARGET_PREFIX):
            if not DELEGATIONS.get((request.resource_id, target_sa)):
                logger.debug("No delegation for resource_id=%s target_sa=%s", request.resource_id, target_sa)
                context.set_code(grpc.StatusCode.PERMISSION_DENIED)
                context.set_details("No delegation for service account " + target_sa)
                return iam_token_service_pb2.CreateIamTokenResponse()
            logger.debug("Delegation found for resource_id=%s target_sa=%s", request.resource_id, target_sa)

        return make_response(token, expires_in)


class OperationServicer(operation_service_pb2_grpc.OperationServiceServicer):
    # operations returned with done=False become done after this many Get calls
    GETS_UNTIL_DONE = 2

    def __init__(self):
        self.gets = {}

    def Get(self, request, context):
        gets = self.gets.get(request.operation_id, 0) + 1
        self.gets[request.operation_id] = gets
        logger.debug("OperationService.Get called, operation_id=%s, gets=%d", request.operation_id, gets)
        return operation_pb2.Operation(id=request.operation_id, done=gets >= self.GETS_UNTIL_DONE)


class ServiceControlServicer(service_control_service_pb2_grpc.ServiceControlServiceServicer):
    def __init__(self):
        self.next_operation = 0

    def _operation(self, done):
        self.next_operation += 1
        return operation_pb2.Operation(id=f"delegation-op-{self.next_operation}", done=done)

    def SetupDelegation(self, request, context):
        target_sa = request.target_service_account_id
        logger.debug(
            "ServiceControlService.SetupDelegation called, service_id=%s microservice_id=%s resource=%s/%s target_sa=%s referrer=%s/%s on_behalf_of=%s",
            request.service_id, request.microservice_id, request.resource.type, request.resource.id, target_sa,
            request.referrer.type, request.referrer.id, request.on_behalf_of_subject_id
        )

        if not check_service_token(context, "ServiceControlService.SetupDelegation"):
            return operation_pb2.Operation()

        # the delegation is authorized for the user who initiated the operation, never for the service itself
        if request.on_behalf_of_subject_id != SUBJECT_ID:
            context.set_code(grpc.StatusCode.PERMISSION_DENIED)
            context.set_details("Subject " + request.on_behalf_of_subject_id + " is not allowed to delegate " + target_sa)
            return operation_pb2.Operation()

        if not request.referrer.id or not request.resource.id:
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details("referrer and resource are required")
            return operation_pb2.Operation()

        if target_sa == 'delegated-bad':
            context.set_code(grpc.StatusCode.PERMISSION_DENIED)
            context.set_details("Reject bad SA")
            return operation_pb2.Operation()

        # the service account lives in another cloud than the resource of the delegation
        if target_sa.startswith('delegated-wrongcloud'):
            fail_with_precondition(context, grpc.StatusCode.FAILED_PRECONDITION,
                                   "Service account " + target_sa + " does not belong to cloud " + request.resource.id,
                                   "BAD_SERVICE_ACCOUNT_CLOUD")
            return operation_pb2.Operation()

        DELEGATIONS.setdefault((request.resource.id, target_sa), set()).add(request.referrer.id)
        return self._operation(done=target_sa != 'delegated-slow')

    def RevokeDelegation(self, request, context):
        target_sa = request.target_service_account_id
        logger.debug(
            "ServiceControlService.RevokeDelegation called, resource=%s/%s target_sa=%s referrer=%s/%s",
            request.resource.type, request.resource.id, target_sa, request.referrer.type, request.referrer.id
        )
        if not check_service_token(context, "ServiceControlService.RevokeDelegation"):
            return operation_pb2.Operation()

        referrers = DELEGATIONS.get((request.resource.id, target_sa))
        if not referrers or request.referrer.id not in referrers:
            context.set_code(grpc.StatusCode.NOT_FOUND)
            context.set_details("No such delegation")
            return operation_pb2.Operation()

        # the delegation exists but IAM refuses to revoke it (tests of the best-effort revoke)
        if target_sa == 'delegated-norevoke':
            context.set_code(grpc.StatusCode.PERMISSION_DENIED)
            context.set_details("Revoke of " + target_sa + " is not allowed")
            return operation_pb2.Operation()

        referrers.discard(request.referrer.id)
        return self._operation(done=True)


class ServiceAccountServicer(service_account_service_pb2_grpc.ServiceAccountServiceServicer):
    def Get(self, request, context):
        sa = request.service_account_id
        logger.debug("ServiceAccountService.Get called, service_account_id=%s", sa)
        if not check_user_token(context, "ServiceAccountService.Get"):
            return service_account_pb2.ServiceAccount()

        if sa.startswith('delegated-nofolder'):
            context.set_code(grpc.StatusCode.NOT_FOUND)
            context.set_details("Service account " + sa + " not found")
            return service_account_pb2.ServiceAccount()

        return service_account_pb2.ServiceAccount(id=sa, folder_id="folder-of-" + sa, name=sa)


class FolderServicer(folder_service_pb2_grpc.FolderServiceServicer):
    def Resolve(self, request, context):
        logger.debug("FolderService.Resolve called, folder_ids=%s", list(request.folder_ids))
        if not check_user_token(context, "FolderService.Resolve"):
            return folder_service_pb2.ResolveFoldersResponse()

        response = folder_service_pb2.ResolveFoldersResponse()
        for folder_id in request.folder_ids:
            if not folder_id.startswith("folder-of-") or folder_id.startswith("folder-of-delegated-nocloud"):
                continue
            sa = folder_id[len("folder-of-"):]
            response.resolved_folders.append(folder_pb2.ResolvedFolder(id=folder_id, cloud_id="cloud-of-" + sa))
        return response


def make_dummy_subject():
    return access_service_pb2.Subject(
        user_account=access_service_pb2.Subject.UserAccount(
            id=SUBJECT_ID,
            federation_id='mock federation'
        )
    )


class AccessServicer(access_service_pb2_grpc.AccessServiceServicer):
    def __init__(self):
        self.calls = 0
        pass

    def Authenticate(self, request, context):
        logger.debug("AccessService Authenticate called")
        return access_service_pb2.AuthenticateResponse(
            subject=make_dummy_subject()
        )

    def Authorize(self, request, context):
        logger.debug("AccessService Authorize called, iam_token=%s, permission=%s, resource_path=%s", request.iam_token, request.permission, repr(request.resource_path))

        if request.permission != 'iam.serviceAccounts.use':
            context.set_code(grpc.StatusCode.NOT_FOUND)
            context.set_details("Don't know about this permission: " + request.permission)
            return access_service_pb2.AuthorizeResponse()

        for resource in request.resource_path:
            if resource.type != 'iam.serviceAccount':
                context.set_code(grpc.StatusCode.PERMISSION_DENIED)
                context.set_details("Don't know about this type: " + resource.type)

            if resource.id == 'unavailable':
                context.set_code(grpc.StatusCode.UNAVAILABLE)
                context.set_details("Too busy to respond forever")
                return access_service_pb2.AuthorizeResponse()

            if resource.id == 'flaky':
                if random.random() < 0.1:
                    context.set_code(grpc.StatusCode.UNAVAILABLE)
                    context.set_details("Too busy to respond for flaky")
                    return access_service_pb2.AuthorizeResponse()

            if resource.id == 'bad-sa':
                context.set_code(grpc.StatusCode.PERMISSION_DENIED)
                context.set_details("This one is bad")
                return access_service_pb2.AuthorizeResponse()

            if resource.id.startswith('bad-sa-skip-'):
                skips = int(resource.id.split('-')[-1])
                self.calls += 1
                self.calls %= skips + 1
                if self.calls == 0:
                    context.set_code(grpc.StatusCode.PERMISSION_DENIED)
                    context.set_details("Reject bad SA")
                    return access_service_pb2.AuthorizeResponse()

            return access_service_pb2.AuthorizeResponse(
                subject=make_dummy_subject()
            )

        context.set_code(grpc.StatusCode.NOT_FOUND)
        context.set_details("Don't know about those resources")
        return access_service_pb2.AuthorizeResponse()


def main():
    args = parse_args()

    token = args.token
    if args.token_from_env is not None:
        token = os.getenv(args.token_from_env)

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
    access_service_pb2_grpc.add_AccessServiceServicer_to_server(
        AccessServicer(),
        server,
    )
    iam_token_service_pb2_grpc.add_IamTokenServiceServicer_to_server(
        IamTokenServicer(token, args.expires_in),
        server,
    )
    service_control_service_pb2_grpc.add_ServiceControlServiceServicer_to_server(
        ServiceControlServicer(),
        server,
    )
    operation_service_pb2_grpc.add_OperationServiceServicer_to_server(
        OperationServicer(),
        server,
    )
    service_account_service_pb2_grpc.add_ServiceAccountServiceServicer_to_server(
        ServiceAccountServicer(),
        server,
    )
    folder_service_pb2_grpc.add_FolderServiceServicer_to_server(
        FolderServicer(),
        server,
    )
    server.add_insecure_port(f"[::]:{args.port}")
    server.start()
    logger.info("IAM gRPC emulator listening on port %d", args.port)
    server.wait_for_termination()


if __name__ == "__main__":
    main()
