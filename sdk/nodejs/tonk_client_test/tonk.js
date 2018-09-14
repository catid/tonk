function ImportABI() {
    if (process.env.DEBUG) {
        return require('./build/Release/tonk.node');
    } catch (err) {
        return require('./build/Debug/tonk.node');
    }
}

const ABI = ImportABI();

/// TonkResult
/// These are the result codes that can be returned from the tonk_*() functions.
const Result = {
    /// Success code
    Success: 0,

    /// A function parameter was invalid
    InvalidInput: 1,

    /// An error occurred during the request
    Error: 2,

    /// Hostname lookup failed
    HostnameLookup: 3,

    /// Message was too large. Check GetStatus() for the maximum allowed size
    MessageTooLarge: 4,

    /// Network library (asio) returned a failure code
    NetworkFailed: 5,

    /// PRNG library (cymric) returned a failure code
    PRNGFailed: 6,

    /// FEC library (siamese) returned a failure code
    FECFailed: 7,

    /// Authenticated peer network data was invalid
    InvalidData: 8,

    /// Connection rejected by remote host
    ConnectionRejected: 9,

    /// Connection ignored by remote host
    ConnectionTimeout: 10,

    /// The application requested a disconnect
    AppRequest: 11,

    /// The remote application requested a disconnect
    RemoteRequest: 12,

    /// The remote application stopped communicating
    RemoteTimeout: 13,

    /// Received network data could not be authenticated
    BogonData: 14,

    /// Out of memory
    OOM: 15
};

if (Object.freeze) {
    Object.freeze(Result);
}

function IsGood(result) {
    return result === Result.Success;
}

function IsFailed(result) {
    return result !== Result.Success;
}

function ResultToString(result) {
    return ABI.tonk_result_to_string(result);
}


//------------------------------------------------------------------------------
// Connection class

function SDKConnection() {
    this.bar = 'test';
};

SDKConnection.prototype.Member = function() {
};


//------------------------------------------------------------------------------
// Socket class

function SDKSocket() {
    this.bar = 'test';
};

SDKSocket.prototype.Member = function() {
};


//------------------------------------------------------------------------------
// Exports

module.exports.SDKConnection = SDKConnection;
module.exports.SDKSocket = SDKSocket;
module.exports.Result = Result;
module.exports.IsGood = IsGood;
module.exports.IsFailed = IsFailed;
module.exports.ResultToString = ResultToString;
