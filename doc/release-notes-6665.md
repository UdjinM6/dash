Updated RPCs
------------

* The input field `ipAndPort` has been renamed to `coreP2PAddrs`.
  * `coreP2PAddrs` can now, in addition to accepting a string, accept an array of strings, subject to validation rules.

* The key `service` has been deprecated for some RPCs (`decoderawtransaction`, `decodepsbt`, `getblock`, `getrawtransaction`,
  `gettransaction`, `masternode status` (only for the `dmnState` key), `protx diff`, `protx listdiff`) and has been replaced
  with the field `addresses`.
  * The deprecated field can be re-enabled using `-deprecatedrpc=service` but is liable to be removed in future versions
    of Dash Core.
  * This change does not affect `masternode status` (except for the `dmnState` key) as `service` does not represent a payload
    value but the external address advertised by the active masternode.
