"""Format view registry: one labelled, explained view per native asset type.

Contract: docs/FORMAT_VIEW_CONTRACT.md. Each decoder module in this package
defines FOURCCS (tuple of type codes it handles), decode(data, context) -> view
dict and, when editable, encode(data, edits) -> bytes. The app inspector, the
MCP server and exporters all call describe()/apply_edits() here, never a
format module directly, so a new decoder appears everywhere at once.
"""
import importlib
import pkgutil

from . import generic

_REGISTRY = None


def _load():
    global _REGISTRY
    if _REGISTRY is None:
        registry = {}
        for info in pkgutil.iter_modules(__path__):
            if info.name.startswith("_") or info.name == "generic":
                continue
            module = importlib.import_module(f"{__name__}.{info.name}")
            for fourcc in getattr(module, "FOURCCS", ()):
                if fourcc in registry:
                    raise RuntimeError(f"Two decoders claim {fourcc}: {registry[fourcc].__name__}, {module.__name__}")
                registry[fourcc] = module
        _REGISTRY = registry
    return _REGISTRY


def decoder_for(fourcc):
    """The module that decodes this type, or None if only the generic view exists."""
    return _load().get(fourcc)


def supported():
    return sorted(_load())


def describe(fourcc, data, context=None):
    """Labelled view of one payload. Falls back to named unknown blocks, never a hex dump."""
    context = dict(context or {})
    module = decoder_for(fourcc)
    if module is None:
        return generic.decode(data, dict(context, fourcc=fourcc))
    try:
        view = module.decode(data, context)
    except (ValueError, IndexError, KeyError, ArithmeticError) as error:
        view = generic.decode(data, dict(context, fourcc=fourcc))
        view["warnings"].append(
            f"This {fourcc} file has a layout the decoder does not recognise ({error}). "
            "It is shown as unknown blocks and saves unchanged.")
    view.setdefault("fourcc", fourcc)
    view.setdefault("warnings", [])
    view.setdefault("blocks", [])
    view.setdefault("fields", [])
    return view


def apply_edits(fourcc, data, edits):
    """Return new payload bytes with {field id: value} edits applied."""
    module = decoder_for(fourcc)
    if module is None or not hasattr(module, "encode"):
        raise ValueError(f"{fourcc} files cannot be edited yet; they open read-only.")
    if not edits:
        return bytes(data)
    return module.encode(data, edits)
