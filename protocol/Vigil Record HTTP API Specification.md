# Vigil Record HTTP API Specification

Created: 2025-10-14

Revised: 2025-10-16

Version: 1.0.3 (Draft)

## Record API

### Record Stats

Get the statistics information of the recording system.

Path: `.../vigil/record/stats`

Method: `GET`

Response Body:

```json
{
    "active": false, // Boolean, true if currently is recording
    "lastActivated": "2025-10-14T07:28:322Z", // String, ISO 8601 format, empty if never recorded
    "lastStatus": 0, // Integer, last stop status, refer to error codes
}
```

### Record Info

Get the extra information of the current recording configuration.

Path: `.../vigil/record/info`

Method: `POST`

Request Payload for `POST`:

```json
{
    "type": "video", // String, options: ["video", "image", "disabled"]
    "data": {
        // For video type
        "clipLength": 60, // Integer, length of each video clip in seconds

        // For image type
        "format": "jpeg" // String, "jpeg" or "png"
    }
}
```

Response Body:

```json
{
    "code": 0, // Integer, error code, refer to error codes
    "message": "", // String, error message, always empty if code is 0
    "quotaBounds": {
        "min": 2147483648, // Integer, minimum quota size in bytes
        "max": 68719476736, // Integer, maximum quota size in bytes
        "step": 1073741824 // Integer, minimum step size for quota adjustment in bytes
    }
}
```

## Trigger API

### Trigger Stats

Get the statistics information of the trigger system.

Path: `.../vigil/trigger/stats`

Method: `GET`

Response Body:

```json
{
    "rules": [
        {
            "uid": "1273d4d9", // String, unique identifier for the rule
            "evaluations": 12345, // Integer, number of times the rule has been evaluated
            "matches": 123, // Integer, number of times the rule has matched
            "lastEvalTime": "2025-10-14T07:28:32Z",  // String, ISO 8601 format, empty if never evaluated
            "lastEvalStatus": 0, // Integer, last evaluation status, refer to error codes
            "lastEvalResult": true // Boolean, last evaluation result, true if matched
        },
        ... // More rules, empty if no rules configured, reconfiguration will reset the stats
    ],
    "gpios": [
        {
            "name": "GPIO 17", // String, GPIO name
            "pin": 17, // Integer, GPIO pin number
            "lastSignal": "high", // String, options ["high", "low", "rising", "falling", "unknown"]
            "lastEvalTime": "2025-10-14T07:28:32Z",  // String, ISO 8601 format, empty if never evaluated
            "lastEvalStatus": 0, // Integer, last evaluation status, refer to error codes
            "lastEvalResult": true // Boolean, last evaluation result, true if matched
        },
        ... // More GPIOs, only list available GPIOs
    ],
    "ttys": [
        {
            "name": "Console", // String, TTY name
            "path": "/dev/shm/vigil-console.sock", // String, TTY device or socket path
            "bufferSize": 4096, // Integer, size of the command buffer in bytes (used to limit the command length)
            "lastEvalTime": "2025-10-14T07:28:32Z",  // String, ISO 8601 format, empty if never evaluated
            "lastEvalStatus": 0, // Integer, last evaluation status, refer to error codes
            "lastEvalResult": true // Boolean, last evaluation result, true if matched
        },
        ... // More TTYs, only list available TTYs
    ]
}
```

### Trigger Info

Get the extra information of the current trigger configuration.

Path: `.../vigil/trigger/info`

Method: `GET`

Response Body:

```json
{
    "gpios": [
        {
            "name": "GPIO 17", // String, GPIO name
            "pin": 17 // Integer, GPIO pin number
        },
        ... // More available GPIOs
    ],
    "ttys": [
        {
            "name": "Console", // String, TTY name
            "path": "/dev/shm/vigil-console.sock" // String, TTY device
        },
        ... // More available TTYs
    ],
    "sourceIds": [
        0 // Integer, available camera source IDs
    ]
}
```

### Trigger Config

Get or set the trigger configuration.

Path: `.../vigil/trigger/config`

Method: `GET` or `POST`

Response Body for `GET` or Request Payload for `POST`:

```json
{
    "type": ..., // String, options ["timer", "gpio", "tty", "ai"]
    "uid": "763d4456", // String, unique identifier for the trigger configuration, used for binding with other configurations
    "data": {
        // For "timer" type
        "interval": 60, // Integer in seconds, range [5, 86400]

        // For "gpio" type
        "name": "GPIO 17", // String, GPIO name
        "signal": "high", // String, options ["high", "low", "rising", "falling"]
        "debounceDuration": 100, // Integer in milliseconds, range [0, 100], apply to both high and low signals

        // For "tty" type
        "name": "Console", // String, TTY name
        "command": "RECORD", // String, command to trigger recording, case-sensitive, control characters (e.g. newline) are not supported

        // For "ai" type
        "rules": [ ... ] // Array of AI Rule Objects, at least one rule is required, if one rule matches (Any), the trigger activates
    }
}
```

Response Body for `POST`:

```json
{
    "code": 0, // Integer, error code, refer to error codes
    "message": "" // String, error message, always empty if code is 0
}
```

While current recording not finished, if the next trigger event comes, the recording will be extended to include the new event, until there's no new events come. This applies to all trigger types since the actual record time (including the time when writing to storage) is uncertain, discarding events during recording may let user unable to capture consecutive events.

#### AI Rule Object

```json
{
    "name": "Rule 1", // String, name of the rule
    "uid": "1273d4d9", // String, unique identifier for the rule, used for stats
    "debounceTimes": 5, // Integer, range [0, 100], change the match result after N consecutive times of the same result
    "sourceIds": [
        0, // Integer, camera source ID
        ...
    ], // Array of integers, camera source IDs to use for this rule
    "classFilter": {
        "classes": [
            {
                "id": 1, // Integer, class ID
                "confidenceRange": [0.6, 1.0] // Array of two floats, range [0.0, 1.0]
            },
            ... // More classes, if one class matches, the filter matches (Any)
        ]
    },
    "regionFilter": {
        "relation": "center", // String, options ["center"], the relation of the bounding box to the regions
        "regions": [
            {
                "polygon": [
                    [0.1, 0.1], // Array of two floats representing a point [x, y], range [0.0, 1.0]
                    [0.5, 0.1],
                    [0.5, 0.5]
                    ... // More points, at least 3 points
                        // - The points should be in normalized device coordinates (NDC)
                        // - The points should form a closed polygon, the last point will be connected to the first point
                        // - The points should be in clockwise or counter-clockwise order
                        // - The origin (0,0) is at the top-left corner, (1,1) is at the bottom-right corner
                ]
            },
            ... // More regions, all regions would be merged as one region (Union)
        ]
    }
}
```

## Scheduler API

### Scheduler Config

Get or set the scheduler configuration.

Path: `.../vigil/scheduler/config`

Method: `GET` or `POST`

Response Body for `GET` or Request Payload for `POST`:

```json
{
    "weekly": [
        {
            "start": {
                "day": 1, // Integer, range [0, 6], 0 = Sunday, 1 = Monday, ..., 6 = Saturday
                "hour": 8, // Integer, range [0, 23]
                "minute": 0 // Integer, range [0, 59]
            },
            "end": {
                "day": 2,
                "hour": 18,
                "minute": 30
            }
        },
        ... // More weekly schedules
    ]
}
```

Response Body for `POST`:

```json
{
    "code": 0, // Integer, error code, refer to error codes
    "message": "" // String, error message, always empty if code is 0
}
```

The start time and end time can be on different days, the schedule will span multiple days, and the schedule is evaluated on the device's local time.

## Storage API

### Storage Info

Get the device or user information and usage statistics of the storage system.

Path: `.../vigil/storage/info`

Method: `GET`

Response Body:

```json
{
    "slots": [
        {
            "name": "Internal Storage", // String, name of the storage slot
            "dev": "/dev/sda1", // String, unique block device path for identifying the storage slot
            "removable": false, // Boolean, true if the storage slot is removable
            "selected": true, // Boolean, true if the storage slot selected for recording
            "status": "normal", // String, options ["error", "formatting", "unmounted", "normal", "syncing", "indexing"]
            "statusAttributes": 0, // Integer, error code if status is error, 0-100 for percentage if status is indexing or formatting
            "fs": "ext4", // String, file system type
            "size": 107374182400, // Integer, size in bytes
            "used": 53687091200, // Integer, used space in bytes
            "available": 53687091200, // Integer, available space in bytes
            "quotaUsed": 42949672960, // Integer, current used size in bytes
            "recordPath": "/DCIM", // String, recording path of the storage slot
        },
        ... // More slots, only list available slots
    ],
    "revisionUid": "763d4456" // String, unique identifier of the current storage & record configuration, changes when configuration is updated
}
```

### Storage Config

Get or set the storage & record configuration.

Path: `.../vigil/storage/config`

Method: `GET` or `POST`

Response Body for `GET` or Request Payload for `POST`:

```json
{
    "selectedSlots": [
        {
            "dev": "/dev/sda1", // String, block device path
            "quotaLimit":  8589934592, // Integer, maximum quota size in bytes, the limit is applied to both image and video recordings
            "quotaRetainAction": "rotate" // String, options ["stop", "rotate"], action when reached the quota or the storage is full
        },
        ... // More selected slots for recording, currently only one slot is supported
    ],
    "recordConfig": {
        "type": "video", // String, options: ["video", "image", "disabled"]
        "data": {
            // For video type
            "clipLength": 60, // Integer, length of each video clip in seconds

            // For image type
            "format": "jpeg" // String, "jpeg" or "png"
        }
    },
    "revisionUid": "763d4456" // String, unique identifier of the current storage & record configuration, changes when configuration is updated
}
```

Response Body for `POST`:

```json
{
    "code": 0, // Integer, error code, refer to error codes
    "message": "" // String, error message, always empty if code is 0
}
```

The `dev` must match an existing storage slot, otherwise an error is returned.

When applying new configuration:

- If the slot is not mounted, it will be formatted with the default file system (ext4 for internal storage, exfat for external storage)
- If the record path does not exist, it will be created
- If the new quota is not applied:
    - If the current quota exceeds the new minimum or maximum quota, an error is returned
    - If it is not a multiple of the minimum quota step, it will be rounded down to the nearest multiple
    - If the current record size exceeds the new maximum quota, the action will be performed immediately
    - If the current record size does not exceed the new maximum quota, the action will be performed when the quota is reached
- Re-indexing of existing recordings/images may happen, it may take a long time if there are many files (async)
- Once all operations are done, the storage slot status will be set to `normal`, otherwise:
    - `error`: means there was an error of the device or file system (e.g. corrupted file system, read-only file system, removed)
    - `formatting`: means the slot is being formatted
    - `unmounted`: means the slot is able to be mounted or removed
    - `syncing`: means the storage manager is writing data to the storage device
    - `indexing`: means the slot is being indexed

For quota retain action in each slot:

- `stop` means stop recording when reached quota or storage is full
- `rotate` means delete old recordings/images when the limit is reached, until enough space is available
    - Old files are determined by the file creation time, the oldest files are deleted first
    - Only files in the record path are considered, other user created files are not affected
    - The detailed rotate mechanism is complex (not going to describe here), but is may always use about 90% of the quota size
    - If all files in record path are deleted and still not enough space, recording will stop, the status will be set to `error`

## Files API

### Files Relay

Get the Nginx relay path of the corresponding storage slots.

Path: `.../vigil/files/relay`

Method: `GET`

Response Body:

```json
{
    "relays": {
        "/dev/sda1": "0", // String, relay token for the storage slot
        ... // More relays, only list available storage slots
    }
}
```

### Files List

List the files in the storage slot.

Path: `.../vigil/files/list`

Method: `POST`

Request Payload:

```json
{
    "slot": 0, // Integer, relay token of the storage slot, refer to Storage Info API
    "path": "", // String, relative path under the record path of the storage slot, default: root of the record path
    "filter": { // Object, optional, filter options
        "nameSuffix": [".jpeg", ".mp4"], // Array of strings, filter by file suffix, empty means no filtering
    },
    "sortBy": "time", // String, options ["name", "time"], sort by name or time, default: "time"
    "sortOrder": "asc", // String, options ["asc", "desc"], ascending or descending order, default: "asc"
    "pageSize": 50, // Integer, number of items per page, range [50, 1000], default: 50
    "currentPage": 0 // Integer, page number, starting from 0, default: 0
}
```

Response Body:

```json
{
    "code": 0, // Integer, error code, refer to error codes
    "message": "", // String, error message, always empty if code is 0
    "totalFiles": 3, // Integer, total number of files
    "totalPages": 1, // Integer, total number of pages
    "currentPage": 0, // Integer, current page number
    "items": [
        "2025-10-14/", // String, directory name ends with '/'
        ... // More items
    ],
}
```

Example of listing files in a directory:

Request Payload:

```json
{
    "slot": 0,
    "path": "2025-10-14/",
    "filter": {
        "nameSuffix": [".jpeg", ".mp4"]
    },
    "sortBy": "time",
    "sortOrder": "asc",
    "pageSize": 50,
    "currentPage": 0
}
```

Response Body:

```json
{
    "code": 0, // Integer, error code, refer to error codes
    "message": "", // String, error message, always empty if code is 0
    "totalPages": 10, // Integer, total number of pages
    "currentPage": 0, // Integer, current page number
    "totalFiles": 486, // Integer, total number of files
    "items": [
        "2025-10-14_15-30-00_1234.jpeg",
        "2025-10-14_15-30-00_1234.mp4",
        ... // More items
    ],
}
```

For security reasons, only files under the record paths of configured storage slots are accessible, and path traversal (e.g. `..`, `~`) is not allowed.

### Files Fetch

Fetch the file content.

Path: `.../vigil/files/<slot-id>/<file-path>`

Method: `GET`

Response Body: Raw file content with appropriate `Content-Type` header.

Note:

- The security considerations mentioned in the Files List parts also apply here
- For large files, it is recommended to use range requests to avoid loading the entire file into memory
- All Files API should support parallel requests, but excessive parallel requests may lead to performance degradation

#### Fetch Image

Query Parameters:

- `w`: Integer, optional, width of the image, range [64, 4096], default: original width
- `h`: Integer, optional, height of the image, range [64, 4096], default: original height

Response Code: `200 OK` on success, otherwise error

Example:

Request: `.../vigil/files/0/2025-10-14/2025-10-14_15-30-00_1234.jpeg?w=800&h=600`

Response Body: Raw JPEG image content with `Content-Type: image/jpeg` header.

Refer to `ngx_http_image_filter_module` for more details on supported sizes and formats.

#### Fetch Video

Request Headers:

- `Range`: optional, byte range request, e.g. `bytes=0-1023`
- Other headers as needed

Response Code: `200 OK` or `206 Partial Content` on success, otherwise error

Example:

Request: `.../vigil/files/0/2025-10-14/2025-10-14_15-30-00_1234.mp4`

Request Headers: `Range: bytes=0-1023`

Response Body: Raw MP4 video content with `Content-Type: video/mp4` header.

Refer to `ngx_http_mp4_module` for more details on MP4 streaming and thumbnail generation.

If the nginx module for thumbnail generation can not meet the performance requirement, a custom thumbnail mechanism may be implemented in the future. e.g. Use `.../vigil/files/0/.cache/2025-10-14/2025-10-14_15-30-00_1234.mp4.thumb.jpeg` to get the thumbnail of the video file.

## Miscellaneous

### Error Codes

| Code | Message               | Description                                      |
|------|-----------------------|--------------------------------------------------|
| 0    | Success               | Operation completed successfully                 |

(other codes to be defined)
