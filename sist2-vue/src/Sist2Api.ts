import axios from "axios";
import {ext, strUnescape, lum} from "./util";
import CryptoES from 'crypto-es';

export interface EsTag {
    id: string
    count: number
    color: string | undefined
    isLeaf: boolean
}

export interface Tag {
    style: string
    text: string
    rawText: string
    fg: string
    bg: string
    userTag: boolean
}

export interface Index {
    name: string
    version: string
    id: string
    idPrefix: string
    timestamp: number
}

export interface EsHit {
    _index: string
    _id: string
    _score: number
    _path_md5: string
    _type: string
    _tags: Tag[]
    _seq: number
    _source: {
        path: string
        size: number
        mime: string
        name: string
        extension: string
        index: string
        _depth: number
        mtime: number
        videoc: string
        audioc: string
        parent: string
        width: number
        height: number
        duration: number
        tag: string[]
        checksum: string
        thumbnail: string
    }
    _props: {
        isSubDocument: boolean
        isImage: boolean
        isGif: boolean
        isVideo: boolean
        isPlayableVideo: boolean
        isPlayableImage: boolean
        isAudio: boolean
        hasThumbnail: boolean
        tnW: number
        tnH: number
    }
    highlight: {
        name: string[] | undefined,
        content: string[] | undefined,
    }
}

function getIdPrefix(indices: Index[], id: string): string {
    for (let i = 4; i < 32; i++) {
        const prefix = id.slice(0, i);

        if (indices.filter(idx => idx.id.slice(0, i) == prefix).length == 1) {
            return prefix;
        }
    }

    return id;
}

export interface EsResult {
    took: number

    hits: {
        // TODO: ES 6.X ?
        total: {
            value: number
        }
        hits: EsHit[]
    }

    aggregations: any
}

class Sist2Api {

    private baseUrl: string

    constructor(baseUrl: string) {
        this.baseUrl = baseUrl;
    }

    getSist2Info(): Promise<any> {
        return axios.get(`${this.baseUrl}i`).then(resp => {
            const indices = resp.data.indices as Index[];

            resp.data.indices = indices.map(idx => {
                return {
                    id: idx.id,
                    name: idx.name,
                    timestamp: idx.timestamp,
                    version: idx.version,
                    idPrefix: getIdPrefix(indices, idx.id)
                } as Index;
            });

            return resp.data;
        })
    }

    setHitProps(hit: EsHit): void {
        hit["_props"] = {} as any;

        const mimeCategory = hit._source.mime == null ? null : hit._source.mime.split("/")[0];

        if ("parent" in hit._source) {
            hit._props.isSubDocument = true;
        }

        if ("thumbnail" in hit._source) {
            hit._props.hasThumbnail = true;
            hit._props.tnW = Number(hit._source.thumbnail.split(",")[0]);
            hit._props.tnH = Number(hit._source.thumbnail.split(",")[1]);
        }

        switch (mimeCategory) {
            case "image":
                if (hit._source.videoc === "gif") {
                    hit._props.isGif = true;
                } else {
                    hit._props.isImage = true;
                }
                if ("width" in hit._source && !hit._props.isSubDocument && hit._source.videoc !== "tiff"
                    && hit._source.videoc !== "raw" && hit._source.videoc !== "ppm") {
                    hit._props.isPlayableImage = true;
                }
                break;
            case "video":
                if ("videoc" in hit._source) {
                    hit._props.isVideo = true;
                }
                if (hit._props.isVideo) {
                    const videoc = hit._source.videoc;
                    const mime = hit._source.mime;

                    hit._props.isPlayableVideo = mime != null &&
                        mime.startsWith("video/") &&
                        !hit._props.isSubDocument &&
                        hit._source.extension !== "mkv" &&
                        hit._source.extension !== "avi" &&
                        hit._source.extension !== "mov" &&
                        videoc !== "hevc" &&
                        videoc !== "mpeg1video" &&
                        videoc !== "mpeg2video" &&
                        videoc !== "wmv3";
                }
                break;
            case "audio":
                if ("audioc" in hit._source && !hit._props.isSubDocument) {
                    hit._props.isAudio = true;
                }
                break;
        }
    }

    setHitTags(hit: EsHit): void {
        const tags = [] as Tag[];

        const mimeCategory = hit._source.mime == null ? null : hit._source.mime.split("/")[0];

        switch (mimeCategory) {
            case "image":
            case "video":
                if ("videoc" in hit._source && hit._source.videoc) {
                    tags.push({
                        style: "video",
                        text: hit._source.videoc.replace(" ", ""),
                        userTag: false
                    } as Tag);
                }
                break
            case "audio":
                if ("audioc" in hit._source && hit._source.audioc) {
                    tags.push({
                        style: "audio",
                        text: hit._source.audioc,
                        userTag: false
                    } as Tag);
                }
                break;
        }

        // User tags
        if ("tag" in hit._source) {
            hit._source.tag.forEach(tag => {
                tags.push(this.createUserTag(tag));
            })
        }

        hit._tags = tags;
    }

    createUserTag(tag: string): Tag {
        const tokens = tag.split(".");

        const colorToken = tokens.pop() as string;

        const bg = colorToken;
        const fg = lum(colorToken) > 50 ? "#000" : "#fff";

        return {
            style: "user",
            fg: fg,
            bg: bg,
            text: tokens.join("."),
            rawText: tag,
            userTag: true,
        } as Tag;
    }

    esQuery(query: any): Promise<EsResult> {
        return axios.post(`${this.baseUrl}es`, query).then(resp => {
            const res = resp.data as EsResult;

            if (res.hits?.hits) {
                res.hits.hits.forEach((hit: EsHit) => {
                    hit["_source"]["name"] = strUnescape(hit["_source"]["name"]);
                    hit["_source"]["path"] = strUnescape(hit["_source"]["path"]);
                    hit["_path_md5"] = CryptoES.MD5(
                        hit["_source"]["path"] +
                        (hit["_source"]["path"] ? "/" : "") +
                        hit["_source"]["name"] + ext(hit)
                    ).toString();

                    this.setHitProps(hit);
                    this.setHitTags(hit);
                });
            }

            return res;
        });
    }

    getMimeTypes() {
        return this.esQuery({
            aggs: {
                mimeTypes: {
                    terms: {
                        field: "mime",
                        size: 10000
                    }
                }
            },
            size: 0,
        }).then(resp => {
            const mimeMap: any[] = [];
            resp["aggregations"]["mimeTypes"]["buckets"].sort((a: any, b: any) => a.key > b.key).forEach((bucket: any) => {
                const tmp = bucket["key"].split("/");
                const category = tmp[0];
                const mime = tmp[1];

                let category_exists = false;

                const child = {
                    "id": bucket["key"],
                    "text": `${mime} (${bucket["doc_count"]})`
                };

                mimeMap.forEach(node => {
                    if (node.text === category) {
                        node.children.push(child);
                        category_exists = true;
                    }
                });

                if (!category_exists) {
                    mimeMap.push({"text": category, children: [child]});
                }
            })

            return mimeMap;
        });
    }

    _createEsTag(tag: string, count: number): EsTag {
        const tokens = tag.split(".");

        if (/.*\.#[0-9a-f]{6}/.test(tag)) {
            return {
                id: tokens.slice(0, -1).join("."),
                color: tokens.pop(),
                isLeaf: true,
                count: count
            };
        }

        return {
            id: tag,
            count: count,
            isLeaf: false,
            color: undefined
        };
    }

    getDocInfo(docId: string) {
        return axios.get(`${this.baseUrl}d/${docId}`);
    }

    getTags() {
        return this.esQuery({
            aggs: {
                tags: {
                    terms: {
                        field: "tag",
                        size: 10000
                    }
                }
            },
            size: 0,
        }).then(resp => {
            const seen = new Set();

            const tags = resp["aggregations"]["tags"]["buckets"]
                .sort((a: any, b: any) => a["key"].localeCompare(b["key"]))
                .map((bucket: any) => this._createEsTag(bucket["key"], bucket["doc_count"]));

            // Remove duplicates (same tag with different color)
            return tags.filter((t: EsTag) => {
                if (seen.has(t.id)) {
                    return false;
                }
                seen.add(t.id);
                return true;
            });
        });
    }

    saveTag(tag: string, hit: EsHit) {
        return axios.post(`${this.baseUrl}tag/` + hit["_source"]["index"], {
            delete: false,
            name: tag,
            doc_id: hit["_id"],
            path_md5: hit._path_md5
        });
    }

    deleteTag(tag: string, hit: EsHit) {
        return axios.post(`${this.baseUrl}tag/` + hit["_source"]["index"], {
            delete: true,
            name: tag,
            doc_id: hit["_id"],
            path_md5: hit._path_md5
        });
    }

    getTreemapCsvUrl(indexId: string) {
        return `${this.baseUrl}s/${indexId}/1`;
    }

    getMimeCsvUrl(indexId: string) {
        return `${this.baseUrl}s/${indexId}/2`;
    }

    getSizeCsv(indexId: string) {
        return `${this.baseUrl}s/${indexId}/3`;
    }

    getDateCsv(indexId: string) {
        return `${this.baseUrl}s/${indexId}/4`;
    }
}

export default new Sist2Api("");